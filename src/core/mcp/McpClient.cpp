#include "core/mcp/McpClient.h"

#include <QTimer>

#include <utility>

#include "core/mcp/JsonRpcLineCodec.h"
#include "core/mcp/McpTransport.h"

McpClient::McpClient(McpTransport* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
    connect(m_transport, &McpTransport::started, this, &McpClient::beginHandshake);
    connect(m_transport, &McpTransport::lineReceived, this, &McpClient::onLine);
    connect(m_transport, &McpTransport::stderrChunk, this, &McpClient::stderrReceived);
    connect(m_transport, &McpTransport::stopped, this, [this](int) {
        m_ready = false;
        failAllPending(QStringLiteral("transport stopped"));
    });
}

void McpClient::start()
{
    m_transport->start();
}

void McpClient::beginHandshake()
{
    const QJsonObject params{
        {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
        {QStringLiteral("clientInfo"),
         QJsonObject{{"name", QStringLiteral("SynthForgeStudio")},
                     {"version", QStringLiteral("0.1.0")}}},
        {QStringLiteral("capabilities"), QJsonObject{}}};
    sendRequest(QStringLiteral("initialize"), params,
                [this](const QJsonValue& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        emit failed(QStringLiteral("initialize failed: %1").arg(
                            error.value(QStringLiteral("message")).toString()));
                        return;
                    }
                    m_transport->sendLine(JsonRpcLineCodec::makeNotification(
                        QStringLiteral("notifications/initialized"), QJsonObject{}));
                    m_ready = true;
                    emit ready(result.toObject()
                                   .value(QStringLiteral("serverInfo"))
                                   .toObject());
                });
}

void McpClient::listTools(std::function<void(const QJsonArray&)> onOk, ReplyCallback onErr)
{
    sendRequest(QStringLiteral("tools/list"), QJsonObject{},
                [onOk, onErr](const QJsonValue& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        if (onErr)
                            onErr(QJsonValue(), error);
                        return;
                    }
                    if (onOk)
                        onOk(result.toObject().value(QStringLiteral("tools")).toArray());
                });
}

void McpClient::callTool(const QString& name, const QJsonObject& arguments,
                         ReplyCallback onDone, const QJsonValue& progressToken)
{
    QJsonObject params{{"name", name}, {"arguments", arguments}};
    if (!progressToken.isUndefined() && !progressToken.isNull()) {
        params.insert(QStringLiteral("_meta"),
                      QJsonObject{{QStringLiteral("progressToken"), progressToken}});
    }
    sendRequest(QStringLiteral("tools/call"), params, std::move(onDone));
}

qint64 McpClient::sendRequest(const QString& method, const QJsonObject& params,
                              ReplyCallback cb)
{
    const qint64 id = ++m_lastId;
    m_pending.insert(id, std::move(cb));
    m_transport->sendLine(JsonRpcLineCodec::makeRequest(id, method, params));

    QTimer::singleShot(m_timeoutMs, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return; // 已被响应或断开处理
        ReplyCallback cb = it.value();
        m_pending.erase(it);
        cb(QJsonValue(), QJsonObject{{"code", -32000},
                                     {"message", QStringLiteral("request timeout")}});
    });
    return id;
}

void McpClient::onLine(const QByteArray& line)
{
    JsonRpcLine msg;
    QString error;
    if (!JsonRpcLineCodec::parse(line, &msg, &error)) {
        emit failed(QStringLiteral("bad line from server: %1").arg(error));
        return;
    }
    if (msg.kind == JsonRpcLine::Kind::Notification) {
        emit notificationReceived(msg.method, msg.params);
        return;
    }
    resolve(msg.id, msg.result, msg.error);
}

void McpClient::resolve(qint64 id, const QJsonValue& result, const QJsonObject& error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    ReplyCallback cb = it.value();
    m_pending.erase(it);
    cb(result, error);
}

void McpClient::failAllPending(const QString& reason)
{
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        it.value()(QJsonValue(),
                   QJsonObject{{"code", -32001}, {"message", reason}});
    }
}
