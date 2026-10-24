#include "mock/MockLlmServer.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>

namespace {

// 把 choices[0].delta 包成一行 SSE：data: {...}\n\n
QByteArray sseFrame(const QJsonObject& delta)
{
    const QJsonObject payload{{"choices", QJsonArray{QJsonObject{{"delta", delta}}}}};
    return QByteArrayLiteral("data: ")
           + QJsonDocument(payload).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\n\n");
}

} // namespace

namespace MockLlm {

QByteArray textChunk(const QString& text)
{
    return sseFrame(QJsonObject{{"content", text}});
}

QByteArray toolCallStartChunk(int index, const QString& id, const QString& name,
                              const QString& argsFragment)
{
    const QJsonObject function{{"name", name}, {"arguments", argsFragment}};
    const QJsonObject call{
        {"index", index}, {"id", id}, {"type", "function"}, {"function", function}};
    return sseFrame(QJsonObject{{"tool_calls", QJsonArray{call}}});
}

QByteArray toolArgsChunk(int index, const QString& argsFragment)
{
    const QJsonObject function{{"arguments", argsFragment}};
    const QJsonObject call{{"index", index}, {"function", function}};
    return sseFrame(QJsonObject{{"tool_calls", QJsonArray{call}}});
}

QByteArray toolCallChunk(const QString& name, const QString& args)
{
    return toolCallStartChunk(0, QStringLiteral("call_0"), name, args);
}

QByteArray doneChunk()
{
    return QByteArrayLiteral("data: [DONE]\n\n");
}

} // namespace MockLlm

MockLlmServer::MockLlmServer(QList<Turn> script, QObject* parent)
    : QObject(parent)
    , m_script(std::move(script))
{
    connect(&m_server, &QTcpServer::newConnection, this, &MockLlmServer::onNewConnection);
}

bool MockLlmServer::listen()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QString MockLlmServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort());
}

LlmConfig MockLlmServer::config(const QString& model) const
{
    return {baseUrl(), QStringLiteral("test-key"), model};
}

QJsonObject MockLlmServer::requestObjectAt(int index) const
{
    return QJsonDocument::fromJson(requestAt(index).toUtf8()).object();
}

void MockLlmServer::onNewConnection()
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
                return; // body 还没收齐，继续等
            socket->disconnect(socket, &QTcpSocket::readyRead, this, nullptr);

            m_paths << QString::fromUtf8(head.split(' ').value(1));
            m_authorizations << [&head]() {
                for (const QByteArray& line : head.split('\n'))
                    if (line.toLower().startsWith("authorization:"))
                        return line.mid(14).trimmed();
                return QByteArray{};
            }();
            m_requests << QString::fromUtf8(body.left(contentLength));
            respond(socket, takeTurn());
        });
    }
}

void MockLlmServer::respond(QTcpSocket* socket, const Turn& turn)
{
    if (turn.status != 200) { // 错误分支：不发 SSE
        socket->write("HTTP/1.1 " + QByteArray::number(turn.status) + " Error\r\n"
                      "Content-Type: application/json\r\nConnection: close\r\n"
                      "Content-Length: "
                      + QByteArray::number(turn.errorBody.size()) + "\r\n\r\n" + turn.errorBody);
        socket->disconnectFromHost();
        return;
    }

    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                  "Cache-Control: no-cache\r\nConnection: close\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n");

    // 先拼成一整条字节流再切片：切点自然会落在 "data:"、JSON、甚至 UTF-8 多字节中间
    auto slices = std::make_shared<QList<QByteArray>>();
    const int step = qMax(1, turn.splitEvery);
    for (const QByteArray& frame : turn.frames)
        for (int i = 0; i < frame.size(); i += step)
            slices->append(frame.mid(i, step));

    auto index = std::make_shared<int>(0);
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [socket, slices, index, pump, turn]() {
        if (*index >= slices->size()) {
            if (!turn.stallAfterFrames) {
                socket->write("0\r\n\r\n"); // chunked 结束
                socket->flush();
                socket->disconnectFromHost();
            }
            return;
        }
        const QByteArray slice = slices->at((*index)++);
        socket->write(QByteArray::number(slice.size(), 16) + "\r\n");
        socket->write(slice);
        socket->write("\r\n");
        socket->flush(); // 不 flush 会被 Qt 攒包，测不到分片
        QTimer::singleShot(turn.frameDelayMs, socket, [pump]() { (*pump)(); });
    };
    (*pump)();
}

MockLlmServer::Turn MockLlmServer::takeTurn()
{
    if (m_next < m_script.size())
        return m_script.at(m_next++);
    Turn exhausted; // 脚本用超：500，避免测试挂死
    exhausted.status = 500;
    return exhausted;
}
