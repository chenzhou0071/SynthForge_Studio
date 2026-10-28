// 本地假云 3D 服务（可独立运行）：无 Key 走通「提交 → 轮询 → 下载 → 预览」全链路。
// 用法：mock_cloud3d_server [--port 8123] [--glb <真实模型路径>]
// 设置页选「自定义」，请求 Base URL 填启动时打印的地址，API Token 留空。

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstring>
#include <memory>
#include <utility>

namespace {

// 一个单面三角形：足够 three.js 的 GLTFLoader 渲染出可见模型
QByteArray buildPlaceholderGlb()
{
    QByteArray json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"buffers":[{"byteLength":42}]})";
    while (json.size() % 4 != 0)
        json.append(' ');

    QByteArray bin;
    const float positions[] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    for (float value : positions) {
        char bytes[4];
        std::memcpy(bytes, &value, 4);
        bin.append(bytes, 4);
    }
    const quint16 indices[] = {0, 1, 2};
    for (quint16 index : indices) {
        char bytes[2];
        std::memcpy(bytes, &index, 2);
        bin.append(bytes, 2);
    }
    while (bin.size() % 4 != 0)
        bin.append('\0');

    QByteArray glb;
    QDataStream out(&glb, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint32(0x46546C67u); // "glTF"
    out << quint32(2);           // version
    out << quint32(12 + 8 + json.size() + 8 + bin.size());
    out << quint32(json.size());
    out << quint32(0x4E4F534Au); // "JSON"
    out.writeRawData(json.constData(), static_cast<int>(json.size()));
    out << quint32(bin.size());
    out << quint32(0x004E4942u); // "BIN"
    out.writeRawData(bin.constData(), static_cast<int>(bin.size()));
    return glb;
}

QByteArray httpResponse(int status, const QByteArray& contentType, const QByteArray& body)
{
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status)
                     + (status == 200 ? " OK" : " Error") + "\r\n";
    out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

class MockCloud3d : public QObject
{
public:
    explicit MockCloud3d(QByteArray glb)
        : m_glb(std::move(glb))
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MockCloud3d::onNewConnection);
    }

    bool listen(quint16 port) { return m_server.listen(QHostAddress::LocalHost, port); }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

private:
    void onNewConnection()
    {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                if (socket->property("handled").toBool())
                    return;
                buffer->append(socket->readAll());
                const int headEnd = buffer->indexOf("\r\n\r\n");
                if (headEnd < 0)
                    return;
                const QByteArray head = buffer->left(headEnd);
                int contentLength = 0;
                for (const QByteArray& line : head.split('\n')) {
                    const QByteArray trimmed = line.trimmed();
                    if (trimmed.toLower().startsWith("content-length:"))
                        contentLength = trimmed.mid(15).trimmed().toInt();
                }
                if (buffer->size() < headEnd + 4 + contentLength)
                    return;

                socket->setProperty("handled", true);
                const QList<QByteArray> requestLine = head.split('\n').value(0).split(' ');
                const QByteArray method = requestLine.value(0).trimmed();
                QByteArray path = requestLine.value(1).trimmed();
                const int query = path.indexOf('?');
                if (query >= 0)
                    path.truncate(query);
                qInfo("[mock] %s %s", method.constData(), path.constData());

                socket->write(route(method, QString::fromUtf8(path)));
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    QByteArray route(const QByteArray& method, const QString& path)
    {
        if (method == "POST" && path == QLatin1String("/task")) {
            const QString taskId = QStringLiteral("mock-%1").arg(++m_taskSeq);
            m_polls.insert(taskId, 0);
            qInfo("[mock] task %s created", taskId.toUtf8().constData());
            return httpResponse(200, "application/json",
                                R"({"task_id":")" + taskId.toUtf8() + R"("})");
        }
        if (method == "GET" && path.startsWith(QLatin1String("/task/"))) {
            const QString taskId = path.mid(6);
            if (!m_polls.contains(taskId))
                return httpResponse(404, "application/json", R"({"error":"unknown task"})");

            // 前两次查询停在 running，第三次成功；给前端留出看进度的窗口
            const int polls = ++m_polls[taskId];
            QJsonObject payload;
            payload.insert(QStringLiteral("status"), polls >= 3 ? QStringLiteral("success")
                                                                : QStringLiteral("running"));
            payload.insert(QStringLiteral("progress"), polls >= 3 ? 100 : polls * 30);
            if (polls >= 3)
                payload.insert(QStringLiteral("model_url"), baseUrl() + QStringLiteral("/model.glb"));
            return httpResponse(200, "application/json",
                                QJsonDocument(payload).toJson(QJsonDocument::Compact));
        }
        if (method == "GET" && path == QLatin1String("/model.glb"))
            return httpResponse(200, "model/gltf-binary", m_glb);
        return httpResponse(404, "application/json", R"({"error":"not found"})");
    }

    QTcpServer m_server;
    QByteArray m_glb;
    QHash<QString, int> m_polls;
    int m_taskSeq = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("本地假云 3D 服务，用于无 Key 验收"));
    parser.addHelpOption();
    const QCommandLineOption portOption({"p", "port"}, QStringLiteral("监听端口，默认 8123"),
                                        QStringLiteral("port"), QStringLiteral("8123"));
    const QCommandLineOption glbOption({"g", "glb"}, QStringLiteral("返回的真实 GLB 文件"),
                                       QStringLiteral("path"));
    parser.addOptions({portOption, glbOption});
    parser.process(app);

    QByteArray glb = buildPlaceholderGlb();
    const QString glbPath = parser.value(glbOption);
    if (!glbPath.isEmpty()) {
        QFile file(glbPath);
        if (file.open(QIODevice::ReadOnly)) {
            glb = file.readAll();
            qInfo("[mock] serving %s (%lld bytes)", qPrintable(glbPath),
                  static_cast<long long>(glb.size()));
        } else {
            qWarning("[mock] cannot open %s, falling back to placeholder", qPrintable(glbPath));
        }
    } else {
        qInfo("[mock] serving built-in placeholder triangle (%lld bytes)",
              static_cast<long long>(glb.size()));
    }

    MockCloud3d server(glb);
    if (!server.listen(static_cast<quint16>(parser.value(portOption).toUInt()))) {
        qCritical("[mock] cannot listen on port %s", qPrintable(parser.value(portOption)));
        return 1;
    }

    qInfo().noquote() << QStringLiteral(
                             "[mock] listening on %1\n"
                             "[mock] 设置 → 3D → 服务商选“自定义”，请求 Base URL 填 %1，API Token 留空\n"
                             "[mock] Ctrl+C 退出")
                             .arg(server.baseUrl());
    return app.exec();
}
