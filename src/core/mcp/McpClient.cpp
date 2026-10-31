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
    connect(m_transport, &McpTransport::stopped, this,
            [this](int) { onTransportStopped(); });
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &McpClient::sendHeartbeat);
}

void McpClient::start()
{
    m_shutdown = false;
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
                        if (m_everReady) { // 崩后重启的桥接尚未就绪：继续退避重试
                            m_transport->stop();
                            scheduleReconnect();
                        }
                        return;
                    }
                    m_transport->sendLine(JsonRpcLineCodec::makeNotification(
                        QStringLiteral("notifications/initialized"), QJsonObject{}));
                    m_ready = true;
                    m_reconnectAttempt = 0;
                    startHeartbeat();
                    const QJsonObject serverInfo =
                        result.toObject().value(QStringLiteral("serverInfo")).toObject();
                    if (m_everReady)
                        emit reconnected();
                    m_everReady = true;
                    emit ready(serverInfo);
                });
}

void McpClient::shutdown()
{
    m_shutdown = true;
    m_heartbeatTimer.stop();
    m_ready = false;
    failAllPending(QStringLiteral("client shutdown"));
    m_transport->stop();
}

void McpClient::prepareRestart()
{
    m_shutdown = false; // 启动失败时仍允许退避重连
    m_reconnectAttempt = 0;
    m_reconnectScheduled = false;
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
                         ReplyCallback onDone, const QJsonValue& progressToken, int timeoutMs)
{
    QJsonObject params{{"name", name}, {"arguments", arguments}};
    if (!progressToken.isUndefined() && !progressToken.isNull()) {
        params.insert(QStringLiteral("_meta"),
                      QJsonObject{{QStringLiteral("progressToken"), progressToken}});
    }
    sendRequest(QStringLiteral("tools/call"), params, std::move(onDone), timeoutMs);
}

qint64 McpClient::sendRequest(const QString& method, const QJsonObject& params,
                              ReplyCallback cb, int timeoutMs)
{
    const qint64 id = ++m_lastId;
    m_pending.insert(id, std::move(cb));
    m_transport->sendLine(JsonRpcLineCodec::makeRequest(id, method, params));

    const int timeout = timeoutMs > 0 ? timeoutMs : m_timeoutMs;
    QTimer::singleShot(timeout, this, [this, id]() {
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

void McpClient::setHeartbeatInterval(int ms)
{
    m_heartbeatIntervalMs = ms;
    if (m_ready)
        startHeartbeat();
}

void McpClient::startHeartbeat()
{
    m_heartbeatTimer.stop();
    if (m_heartbeatIntervalMs > 0)
        m_heartbeatTimer.start(m_heartbeatIntervalMs);
}

void McpClient::sendHeartbeat()
{
    if (!m_ready || m_heartbeatInFlight)
        return;
    m_heartbeatInFlight = true;
    sendRequest(QStringLiteral("ping"), QJsonObject{},
                [this](const QJsonValue&, const QJsonObject& error) {
                    m_heartbeatInFlight = false;
                    if (error.value(QStringLiteral("code")).toInt() != -32000)
                        return; // 有响应（含服务端错误码）说明连接仍活着
                    m_ready = false;
                    m_heartbeatTimer.stop();
                    failAllPending(QStringLiteral("heartbeat timeout"));
                    m_transport->stop(); // 进程卡死时先杀掉，否则 restart 会失败
                    scheduleReconnect();
                },
                m_heartbeatTimeoutMs);
}

void McpClient::onTransportStopped()
{
    const bool wasReady = m_ready;
    m_ready = false;
    m_heartbeatTimer.stop();
    m_heartbeatInFlight = false;
    failAllPending(QStringLiteral("transport stopped"));
    if (m_shutdown)
        return; // 主动 shutdown：不重连、不报错
    if (wasReady) {
        emit failed(QStringLiteral("MCP 桥接异常退出，正在自动重连"));
        scheduleReconnect();
    } else if (m_everReady) {
        scheduleReconnect(); // 重连/手动 restart 的进程没起来：继续退避，不让灯卡死在“重连中”
    }
}

void McpClient::scheduleReconnect()
{
    if (m_reconnectScheduled || m_shutdown)
        return;
    m_reconnectScheduled = true;
    ++m_reconnectAttempt;
    emit reconnecting(m_reconnectAttempt);
    const int delayMs = qMin(1000 * (1 << qMin(m_reconnectAttempt - 1, 5)), 30000);
    QTimer::singleShot(delayMs, this, [this]() {
        m_reconnectScheduled = false;
        if (m_shutdown || m_ready)
            return; // 已恢复就绪时，陈旧定时器不得再断开健康连接
        m_transport->stop(); // 幂等：NotRunning 时直接返回
        m_transport->start(); // started → McpClient 内部 beginHandshake 自动重新握手
    });
}
