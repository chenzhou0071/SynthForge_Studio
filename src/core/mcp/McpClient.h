#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class McpTransport;

class McpClient : public QObject
{
    Q_OBJECT

public:
    // error 为空对象表示成功；result 的语义由调用方解释。
    using ReplyCallback = std::function<void(const QJsonValue& result, const QJsonObject& error)>;

    explicit McpClient(McpTransport* transport, QObject* parent = nullptr);

    void start(); // 启动传输；started 后自动握手（重连也走这里）
    void shutdown(); // 主动停止：关心跳、停 transport、不触发自动重连
    void prepareRestart(); // bridge.restart：解除 shutdown 与退避计数，随后调用 start()
    bool isReady() const { return m_ready; }
    void setRequestTimeout(int ms) { m_timeoutMs = ms; }
    void setHeartbeatInterval(int ms); // <= 0 关闭心跳；默认 30000
    void setHeartbeatTimeout(int ms) { m_heartbeatTimeoutMs = ms; } // 默认 5000

    void listTools(std::function<void(const QJsonArray& tools)> onOk,
                   ReplyCallback onErr = nullptr);
    void callTool(const QString& name, const QJsonObject& arguments,
                  ReplyCallback onDone,
                  const QJsonValue& progressToken = QJsonValue());

signals:
    void ready(const QJsonObject& serverInfo);
    void failed(const QString& reason);
    void reconnecting(int attempt); // attempt 从 1 起
    void reconnected();             // 仅断线后的重新握手成功才发射
    void notificationReceived(const QString& method, const QJsonValue& params);
    void stderrReceived(const QByteArray& chunk);

private:
    void beginHandshake();
    qint64 sendRequest(const QString& method, const QJsonObject& params, ReplyCallback cb,
                       int timeoutMs = -1);
    void onLine(const QByteArray& line);
    void resolve(qint64 id, const QJsonValue& result, const QJsonObject& error);
    void failAllPending(const QString& reason);
    void onTransportStopped();
    void startHeartbeat();
    void sendHeartbeat();
    void scheduleReconnect();

    McpTransport* m_transport;
    QHash<qint64, ReplyCallback> m_pending;
    qint64 m_lastId = 0;
    int m_timeoutMs = 30000;
    bool m_ready = false;

    QTimer m_heartbeatTimer;
    int m_heartbeatIntervalMs = 30000;
    int m_heartbeatTimeoutMs = 5000;
    bool m_heartbeatInFlight = false;

    bool m_everReady = false; // 首次握手成功后才允许自动重连
    bool m_shutdown = false;
    bool m_reconnectScheduled = false;
    int m_reconnectAttempt = 0;
};
