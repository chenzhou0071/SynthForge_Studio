#include <QJsonArray>
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

class ReconnectTest : public QObject
{
    Q_OBJECT

private slots:
    void crashedBridgeIsRestartedAndRehandshaked()
    {
        BridgeCommand command{mockServerProgram(), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);

        QSignalSpy readySpy(&client, &McpClient::ready);
        client.start();
        QVERIFY2(readySpy.wait(15000), "首次握手失败");

        QSignalSpy reconnectingSpy(&client, &McpClient::reconnecting);
        QSignalSpy reconnectedSpy(&client, &McpClient::reconnected);

        transport.stop(); // 模拟桥接进程崩溃（McpClient 视角为意外断开）

        QTRY_VERIFY_WITH_TIMEOUT(reconnectingSpy.count() >= 1, 5000);
        QCOMPARE(reconnectingSpy.first().at(0).toInt(), 1);

        QVERIFY2(reconnectedSpy.wait(15000), "崩溃后未自动重连并重新握手");

        QJsonArray tools;
        bool listed = false;
        client.listTools([&](const QJsonArray& t) {
            tools = t;
            listed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(listed, 15000);
        QCOMPARE(tools.size(), 1);

        client.shutdown();
    }

    void stalledBridgeIsDetectedByHeartbeat()
    {
        qputenv("MOCK_IGNORE_PING", "1"); // mock 收到 ping 不回复
        BridgeCommand command{mockServerProgram(), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);
        client.setHeartbeatInterval(200);
        client.setHeartbeatTimeout(500);

        QSignalSpy readySpy(&client, &McpClient::ready);
        client.start();
        QVERIFY2(readySpy.wait(15000), "首次握手失败");

        QSignalSpy reconnectingSpy(&client, &McpClient::reconnecting);
        QTRY_VERIFY_WITH_TIMEOUT(reconnectingSpy.count() >= 1, 10000);

        client.shutdown();
        qunsetenv("MOCK_IGNORE_PING");
    }
};

QTEST_MAIN(ReconnectTest)
#include "tst_reconnect.moc"
