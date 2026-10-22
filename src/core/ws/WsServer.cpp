#include "core/ws/WsServer.h"

#include "core/ws/ProtocolRouter.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <QWebSocket>

WsServer::WsServer(ProtocolRouter* router, QObject* parent)
    : QObject(parent)
    , m_server(QStringLiteral("SynthForge"), QWebSocketServer::NonSecureMode)
    , m_router(router)
    , m_token(QString::fromLatin1(
          QCryptographicHash::hash(
              QByteArray::number(QRandomGenerator::system()->generate64()),
              QCryptographicHash::Sha256)
              .toHex()))
{
    connect(&m_server, &QWebSocketServer::newConnection, this, &WsServer::onNewConnection);
}

bool WsServer::listen(quint16 port)
{
    return m_server.listen(QHostAddress::LocalHost, port);
}

void WsServer::onNewConnection()
{
    while (QWebSocket* socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        const QString presented = QUrlQuery(socket->requestUrl()).queryItemValue(
            QStringLiteral("token"), QUrl::FullyDecoded);
        if (presented != m_token) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("bad token"));
            socket->deleteLater();
            continue;
        }
        m_clients.append(socket);
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { onTextMessage(socket, text); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            m_clients.removeAll(socket);
            socket->deleteLater();
        });
    }
}

void WsServer::onTextMessage(QWebSocket* socket, const QString& text)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (!doc.isObject()) {
        sendError(socket, QJsonValue(), -32700, QStringLiteral("parse error"));
        return;
    }

    const QJsonObject frame = doc.object();
    const QJsonValue id = frame.value(QStringLiteral("id"));
    const QString method = frame.value(QStringLiteral("method")).toString();
    try {
        const QJsonValue result =
            m_router->dispatch(method, frame.value(QStringLiteral("params")).toObject());
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                      {QStringLiteral("result"), result}})
                .toJson(QJsonDocument::Compact)));
    } catch (const std::exception& e) {
        sendError(socket, id, -32000, QString::fromUtf8(e.what()));
    }
}

void WsServer::sendError(QWebSocket* socket, const QJsonValue& id, int code,
                         const QString& message) const
{
    socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(QJsonObject{
                          {QStringLiteral("id"), id},
                          {QStringLiteral("error"),
                           QJsonObject{{QStringLiteral("code"), code},
                                       {QStringLiteral("message"), message}}}})
            .toJson(QJsonDocument::Compact)));
}

void WsServer::broadcastEvent(const QString& event, const QJsonObject& data) const
{
    const QString payload = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("event"), event},
                                  {QStringLiteral("data"), data}})
            .toJson(QJsonDocument::Compact));
    for (QWebSocket* client : m_clients)
        client->sendTextMessage(payload);
}
