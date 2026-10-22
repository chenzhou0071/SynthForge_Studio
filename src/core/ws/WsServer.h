#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QWebSocketServer>

class ProtocolRouter;
class QWebSocket;

class WsServer : public QObject
{
    Q_OBJECT

public:
    explicit WsServer(ProtocolRouter* router, QObject* parent = nullptr);
    bool listen(quint16 port = 0); // 只监听 127.0.0.1
    quint16 port() const { return m_server.serverPort(); }
    QString token() const { return m_token; }
    void broadcastEvent(const QString& event, const QJsonObject& data) const;

private:
    void onNewConnection();
    void onTextMessage(QWebSocket* socket, const QString& text);
    void sendError(QWebSocket* socket, const QJsonValue& id, int code,
                   const QString& message) const;

    QWebSocketServer m_server;
    ProtocolRouter* m_router;
    QString m_token;
    QList<QWebSocket*> m_clients;
};
