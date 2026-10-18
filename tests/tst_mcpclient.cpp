#include <QJsonDocument>
#include <QtTest>
#include "core/mcp/McpClient.h"
#include "core/mcp/McpTransport.h"

// 内存传输替身：不发真实进程，手动喂入服务端行数据。
class FakeTransport : public McpTransport
{
public:
    void start() override
    {
        ++startCount;
        emit started();
    }
    void sendLine(const QByteArray& line) override { sent.append(line); }
    void stop() override { emit stopped(0); }

    QList<QByteArray> sent;
    int startCount = 0;

    void feed(const QByteArray& json) { emit lineReceived(json); }
    void feedStderr(const QByteArray& chunk) { emit stderrChunk(chunk); }
};

class McpClientTest : public QObject
{
    Q_OBJECT

private slots:
    void handshakeSucceeds()
    {
        FakeTransport transport;
        McpClient client(&transport);
        QSignalSpy readySpy(&client, &McpClient::ready);

        client.start();

        QCOMPARE(transport.startCount, 1);
        QCOMPARE(transport.sent.size(), 1);
        QVERIFY(transport.sent.first().contains("initialize"));
        QVERIFY(transport.sent.first().contains("2025-06-18"));

        transport.feed(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","serverInfo":{"name":"mock","version":"0.0.1"},"capabilities":{}}})");

        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(transport.sent.size(), 2);
        QVERIFY(transport.sent.at(1).contains("notifications/initialized"));
        QVERIFY(client.isReady());
    }

    void initializeErrorEmitsFailed()
    {
        FakeTransport transport;
        McpClient client(&transport);
        QSignalSpy failedSpy(&client, &McpClient::failed);

        client.start();
        transport.feed(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"bad params"}})");

        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(!client.isReady());
    }

    void outOfOrderResponsesMatchById()
    {
        FakeTransport transport;
        McpClient client(&transport);

        QJsonArray toolsResult;
        QJsonValue callResult;
        client.listTools([&](const QJsonArray& tools) { toolsResult = tools; });
        client.callTool(QStringLiteral("echo"),
                        QJsonObject{{"text", "hi"}},
                        [&](const QJsonValue& r, const QJsonObject&) { callResult = r; });

        // 先回第二个请求（id=2），再回第一个（id=1）
        transport.feed(R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"hi"}]}})");
        transport.feed(R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"echo"}]}})");

        QCOMPARE(callResult.toObject().value(QStringLiteral("content")).toArray().size(), 1);
        QCOMPARE(toolsResult.size(), 1);
        QCOMPARE(toolsResult.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("echo"));
    }

    void requestTimeoutDeliversError()
    {
        FakeTransport transport;
        McpClient client(&transport);
        client.setRequestTimeout(30);

        QJsonObject receivedError;
        bool done = false;
        client.callTool(QStringLiteral("slow"), QJsonObject{},
                        [&](const QJsonValue&, const QJsonObject& error) {
                            receivedError = error;
                            done = true;
                        });

        QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
        QCOMPARE(receivedError.value(QStringLiteral("code")).toInt(), -32000);
    }

    void callToolSendsProgressToken()
    {
        FakeTransport transport;
        McpClient client(&transport);

        client.callTool(QStringLiteral("slow"), QJsonObject{},
                        [](const QJsonValue&, const QJsonObject&) {},
                        QStringLiteral("tok-1"));

        const QJsonObject sent = QJsonDocument::fromJson(transport.sent.last()).object();
        const QJsonObject params = sent.value(QStringLiteral("params")).toObject();
        QCOMPARE(params.value(QStringLiteral("_meta"))
                     .toObject()
                     .value(QStringLiteral("progressToken"))
                     .toString(),
                 QStringLiteral("tok-1"));
    }

    void forwardsNotifications()
    {
        FakeTransport transport;
        McpClient client(&transport);

        QString method;
        QJsonValue params;
        connect(&client, &McpClient::notificationReceived, this,
                [&](const QString& m, const QJsonValue& p) {
                    method = m;
                    params = p;
                });

        transport.feed(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"t","progress":0.5}})");

        QCOMPARE(method, QStringLiteral("notifications/progress"));
        QCOMPARE(params.toObject().value(QStringLiteral("progress")).toDouble(), 0.5);
    }

    void forwardsStderr()
    {
        FakeTransport transport;
        McpClient client(&transport);

        QByteArray received;
        connect(&client, &McpClient::stderrReceived, this,
                [&](const QByteArray& chunk) { received = chunk; });

        transport.feedStderr("bridge boom\n");

        QCOMPARE(received, QByteArray("bridge boom\n"));
    }

    void transportStopFailsPending()
    {
        FakeTransport transport;
        McpClient client(&transport);
        client.setRequestTimeout(60000);

        QJsonObject receivedError;
        bool done = false;
        client.callTool(QStringLiteral("x"), QJsonObject{},
                        [&](const QJsonValue&, const QJsonObject& error) {
                            receivedError = error;
                            done = true;
                        });

        transport.stop();
        QVERIFY(done);
        QCOMPARE(receivedError.value(QStringLiteral("code")).toInt(), -32001);
    }
};

QTEST_MAIN(McpClientTest)
#include "tst_mcpclient.moc"
