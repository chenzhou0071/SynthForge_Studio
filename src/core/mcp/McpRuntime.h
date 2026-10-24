#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

#include <memory>

class McpClient;
class McpTransport;

class McpRuntime : public QObject
{
    Q_OBJECT

public:
    McpRuntime(const QString& area, std::unique_ptr<McpTransport> transport,
               QObject* parent = nullptr);
    ~McpRuntime() override;

    bool start();
    bool restart(); // 主动重启：不触发自动重连的退避，由调用方直接拉起
    void stop();    // 主动断开：关心跳，供切换工程；再次 start() 才恢复
    McpClient* client() const { return m_client; }
    bool isReady() const;
    QJsonArray tools() const { return m_tools; }
    void refreshTools();

signals:
    void toolsChanged(const QJsonArray& tools);
    void statusChanged(const QString& area, const QString& status, const QString& hint);

private:
    QString m_area;
    std::unique_ptr<McpTransport> m_transport;
    McpClient* m_client;
    QJsonArray m_tools;
};
