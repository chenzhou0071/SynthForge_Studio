#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>

class QTcpSocket;

// 本地假云 3D 服务：按 method+path 逐条吐响应，并记录收到的请求。
// 只实现我们用到的最小 HTTP/1.1 子集，用于真实走一遍 QNetworkAccessManager。
class MockCloud3dServer : public QObject
{
    Q_OBJECT
public:
    struct Request
    {
        QByteArray method;
        QString path;
        QByteArray head;
        QByteArray body;
    };

    explicit MockCloud3dServer(QObject* parent = nullptr);

    bool listen();
    QString baseUrl() const;

    // 同一路由可多次 enqueue，按先后顺序消费；没有脚本的请求回 404
    void enqueue(const QByteArray& method, const QString& path, int status,
                 const QByteArray& body);

    const QList<Request>& requests() const { return m_requests; }

private:
    struct ScriptedReply
    {
        QByteArray method;
        QString path;
        int status;
        QByteArray body;
    };

    void onNewConnection();
    ScriptedReply takeReply(const QByteArray& method, const QString& path);

    QTcpServer m_server;
    QList<Request> m_requests;
    QList<ScriptedReply> m_script;
};
