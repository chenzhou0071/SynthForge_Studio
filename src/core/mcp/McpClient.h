#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

class McpTransport;

class McpClient : public QObject
{
    Q_OBJECT

public:
    // error 为空对象表示成功；result 的语义由调用方解释。
    using ReplyCallback = std::function<void(const QJsonValue& result, const QJsonObject& error)>;

    explicit McpClient(McpTransport* transport, QObject* parent = nullptr);

    void start(); // 启动传输；transport 的 started 信号会触发 initialize 握手
    bool isReady() const { return m_ready; }
    void setRequestTimeout(int ms) { m_timeoutMs = ms; }

    void listTools(std::function<void(const QJsonArray& tools)> onOk,
                   ReplyCallback onErr = nullptr);
    void callTool(const QString& name, const QJsonObject& arguments,
                  ReplyCallback onDone,
                  const QJsonValue& progressToken = QJsonValue());

signals:
    void ready(const QJsonObject& serverInfo);
    void failed(const QString& reason);
    void notificationReceived(const QString& method, const QJsonValue& params);
    void stderrReceived(const QByteArray& chunk);

private:
    void beginHandshake();
    qint64 sendRequest(const QString& method, const QJsonObject& params, ReplyCallback cb);
    void onLine(const QByteArray& line);
    void resolve(qint64 id, const QJsonValue& result, const QJsonObject& error);
    void failAllPending(const QString& reason);

    McpTransport* m_transport;
    QHash<qint64, ReplyCallback> m_pending;
    qint64 m_lastId = 0;
    int m_timeoutMs = 30000;
    bool m_ready = false;
};
