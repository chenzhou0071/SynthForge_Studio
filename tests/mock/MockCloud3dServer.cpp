#include "mock/MockCloud3dServer.h"

#include <QTcpSocket>

#include <memory>

MockCloud3dServer::MockCloud3dServer(QObject* parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &MockCloud3dServer::onNewConnection);
}

bool MockCloud3dServer::listen()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QString MockCloud3dServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
}

void MockCloud3dServer::enqueue(const QByteArray& method, const QString& path, int status,
                                const QByteArray& body)
{
    m_script.append({method, path, status, body});
}

void MockCloud3dServer::onNewConnection()
{
    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return; // 头还没收齐
            const QByteArray head = buffer->left(headerEnd);
            int contentLength = 0;
            for (const QByteArray& line : head.split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    contentLength = line.mid(15).trimmed().toInt();
            }
            const QByteArray body = buffer->mid(headerEnd + 4);
            if (body.size() < contentLength)
                return; // body 还没收齐
            socket->disconnect(socket, &QTcpSocket::readyRead, this, nullptr);

            const QList<QByteArray> requestLine =
                head.split('\n').value(0).trimmed().split(' ');
            const QByteArray method = requestLine.value(0);
            const QString path = QString::fromUtf8(requestLine.value(1));
            m_requests.append({method, path, head, body.left(contentLength)});

            const ScriptedReply reply = takeReply(method, path);
            const char* statusText = reply.status == 200 ? "OK" : "Error";
            socket->write("HTTP/1.1 " + QByteArray::number(reply.status) + " " + statusText
                          + "\r\nContent-Type: application/json\r\nConnection: close\r\n"
                            "Content-Length: "
                          + QByteArray::number(reply.body.size()) + "\r\n\r\n" + reply.body);
            socket->disconnectFromHost();
        });
    }
}

MockCloud3dServer::ScriptedReply MockCloud3dServer::takeReply(const QByteArray& method,
                                                             const QString& path)
{
    for (int i = 0; i < m_script.size(); ++i) {
        const ScriptedReply& candidate = m_script.at(i);
        if (candidate.method == method && candidate.path == path)
            return m_script.takeAt(i);
    }
    return {method, path, 404, QByteArray(R"({"message":"no scripted reply"})")};
}
