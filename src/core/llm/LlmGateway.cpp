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
                            const QJsonArray& tools, StreamHandlers handlers)
{
    QJsonObject body{{"model", cfg.model}, {"messages", messages}, {"stream", true}};
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
        process(state->parser.feed(reply->readAll()));
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, state, process, handlers]() {
                m_replies.remove(reply);
                if (reply->error() != QNetworkReply::NoError) {
                    if (!state->done && handlers.onError)
                        handlers.onError(reply->errorString());
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
