#include "core/llm/LlmGateway.h"

#include "core/llm/SseParser.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

struct StreamState
{
    SseParser parser;
    QHash<int, QJsonObject> toolCalls; // index -> {id, name, arguments(字符串累加)}
    QByteArray rawHead;                // 截存响应开头字节：出错时 body 已被 SSE 解析器吃掉，靠它还原原因
    bool done = false;
};

void finishStream(const std::shared_ptr<StreamState>& state,
                  const LlmGateway::StreamHandlers& handlers)
{
    if (state->done)
        return;
    state->done = true;

    QJsonArray calls;
    QList<int> indexes = state->toolCalls.keys();
    std::sort(indexes.begin(), indexes.end()); // QHash 顺序无保证，按 index 还原
    for (int index : indexes) {
        QJsonObject call = state->toolCalls.value(index);
        if (!call.contains(QStringLiteral("id")))
            call.insert(QStringLiteral("id"), QStringLiteral("call_%1").arg(index));
        call.insert(QStringLiteral("index"), index);

        QJsonParseError error;
        const QJsonDocument arguments =
            QJsonDocument::fromJson(call.value(QStringLiteral("arguments")).toString().toUtf8(),
                                    &error);
        // 解析失败按空对象处理，避免整轮任务失败
        call.insert(QStringLiteral("arguments"),
                    error.error == QJsonParseError::NoError && arguments.isObject()
                        ? arguments.object()
                        : QJsonObject{});
        calls.append(call);
    }

    if (!calls.isEmpty() && handlers.onToolCalls)
        handlers.onToolCalls(calls);
    if (handlers.onDone)
        handlers.onDone();
}

} // namespace

LlmGateway::LlmGateway(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void LlmGateway::chatStream(const LlmConfig& cfg, const QJsonArray& messages,
                            const QJsonArray& tools, StreamHandlers handlers,
                            const QString& tag)
{
    QJsonObject body{{"model", cfg.model}, {"messages", messages}, {"stream", true}};
    // 显式关闭思考模式：DeepSeek 新模型（flash 等）默认开启 thinking，会在多轮工具回喂时
    // 强制要求把 reasoning_content 原样传回，否则 400。本项目按非思考用，直接关掉。
    body.insert(QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"), QStringLiteral("disabled")}});
    if (!tools.isEmpty())
        body.insert(QStringLiteral("tools"), tools); // 已是 OpenAI tools，转换由调用方经 ToolBridge 完成

    QString baseUrl = cfg.baseUrl;
    while (baseUrl.endsWith(QLatin1Char('/')))
        baseUrl.chop(1);

    QNetworkRequest request(QUrl(baseUrl + QStringLiteral("/chat/completions")));
    request.setRawHeader("Authorization", "Bearer " + cfg.apiKey.toUtf8());
    request.setRawHeader("Accept", "text/event-stream");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply* reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_replies.insert(reply);
    m_tags.insert(reply, tag);

    auto state = std::make_shared<StreamState>();
    auto process = [state, handlers](const QList<SseParser::Event>& events) {
        for (const auto& event : events) {
            if (state->done)
                return;
            if (event.data == "[DONE]") {
                finishStream(state, handlers);
                return;
            }

            const QJsonObject chunk = QJsonDocument::fromJson(event.data).object();
            const QJsonArray choices = chunk.value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty())
                continue; // usage 等无 choices 的尾帧
            const QJsonObject delta =
                choices.first().toObject().value(QStringLiteral("delta")).toObject();

            const QJsonValue content = delta.value(QStringLiteral("content"));
            if (content.isString() && handlers.onDelta)
                handlers.onDelta(content.toString());

            for (const QJsonValue& value :
                 delta.value(QStringLiteral("tool_calls")).toArray()) {
                const QJsonObject obj = value.toObject();
                const int index = obj.value(QStringLiteral("index")).toInt();
                QJsonObject& slot = state->toolCalls[index];
                if (obj.value(QStringLiteral("id")).isString())
                    slot.insert(QStringLiteral("id"),
                                obj.value(QStringLiteral("id")).toString());
                const QJsonObject fn = obj.value(QStringLiteral("function")).toObject();
                if (fn.value(QStringLiteral("name")).isString())
                    slot.insert(QStringLiteral("name"),
                                fn.value(QStringLiteral("name")).toString());
                slot.insert(QStringLiteral("arguments"),
                            slot.value(QStringLiteral("arguments")).toString()
                                + fn.value(QStringLiteral("arguments")).toString());
            }
        }
    };

    connect(reply, &QNetworkReply::readyRead, this, [reply, state, process]() {
        const QByteArray chunk = reply->readAll();
        if (state->rawHead.size() < 4096)
            state->rawHead.append(chunk); // 只留开头 4KB：够装下错误 body，又不给正常长流占内存
        process(state->parser.feed(chunk));
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, state, process, handlers]() {
                m_replies.remove(reply);
                m_tags.remove(reply);
                if (reply->error() != QNetworkReply::NoError) {
                    if (!state->done && handlers.onError) {
                        // 只报 errorString 会丢掉服务端真正的原因（400 body 里写着哪条消息不合法）；
                        // body 已被 readyRead 喂进 SSE 解析器，靠 rawHead 还原
                        const QString detail = QString::fromUtf8(state->rawHead).trimmed();
                        handlers.onError(detail.isEmpty()
                                             ? reply->errorString()
                                             : QStringLiteral("%1：%2")
                                                   .arg(reply->errorString(), detail));
                    }
                } else if (!state->done) {
                    // 有些服务端最后一帧不带空行：补一个帧结束符冲出来
                    process(state->parser.feed("\n\n"));
                    finishStream(state, handlers);
                }
                reply->deleteLater();
            });
}

void LlmGateway::abort()
{
    const QSet<QNetworkReply*> replies = m_replies;
    for (QNetworkReply* reply : replies)
        reply->abort();
}

void LlmGateway::abortTag(const QString& tag)
{
    // 先收集再 abort：abort 会触发 finished，直接遍历会被 remove 掉元素
    QList<QNetworkReply*> replies;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
        if (it.value() == tag)
            replies.append(it.key());
    for (QNetworkReply* reply : replies)
        reply->abort();
}
