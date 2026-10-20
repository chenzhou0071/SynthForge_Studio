#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QtTest>

#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"

namespace {

// mock server 与测试 exe 同目录；不用 CMake 宏传路径，
// 因为 Windows 反斜杠会被 C++ 当成 \p、\b 等转义序列。
QString mockServerProgram()
{
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server");
#endif
}

} // namespace

class StdioTransportTest : public QObject
{
    Q_OBJECT

private slots:
    void endToEndThroughMockServer()
    {
        // mock server 把每个响应拆成两次 write（中间 sleep），
        // 因此这个用例同时验证跨 chunk 的半行拼接。
        BridgeCommand command{mockServerProgram(), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);

        QSignalSpy readySpy(&client, &McpClient::ready);
        client.start();
        QVERIFY2(readySpy.wait(15000), "mock server 未在 15 秒内完成握手");

        QJsonArray tools;
        bool listed = false;
        client.listTools([&](const QJsonArray& t) {
            tools = t;
            listed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(listed, 15000);
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("echo"));

        QString echoed;
        bool called = false;
        client.callTool(QStringLiteral("echo"), QJsonObject{{"text", "hello synthforge"}},
                        [&](const QJsonValue& result, const QJsonObject& error) {
                            if (error.isEmpty()) {
                                echoed = result.toObject()
                                             .value(QStringLiteral("content"))
                                             .toArray()
                                             .first()
                                             .toObject()
                                             .value(QStringLiteral("text"))
                                             .toString();
                            }
                            called = true;
                        });
        QTRY_VERIFY_WITH_TIMEOUT(called, 15000);
        QCOMPARE(echoed, QStringLiteral("hello synthforge"));

        QSignalSpy stoppedSpy(&transport, &McpTransport::stopped);
        transport.stop();
        QVERIFY2(stoppedSpy.wait(5000), "mock server 未在 5 秒内退出");
    }
};

QTEST_MAIN(StdioTransportTest)
#include "tst_stdio_transport.moc"
