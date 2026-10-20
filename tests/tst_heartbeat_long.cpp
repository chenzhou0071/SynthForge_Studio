#include <QProcessEnvironment>
#include <QtTest>

#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"

namespace {

QString mockServerProgram()
{
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server");
#endif
}

} // namespace

// 慢测试：用真实默认节奏（30 秒间隔 / 5 秒超时）跑够两轮心跳，约 65 秒。
// 默认会进 ctest；只想跑快测试时用 ctest -LE slow 跳过。
class HeartbeatLongTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultCadenceSendsTwoHeartbeats()
    {
        BridgeCommand command{mockServerProgram(), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);
        // 不改心跳参数：走默认 30000ms / 5000ms

        QByteArray stderrBuffer;
        QObject::connect(&client, &McpClient::stderrReceived, &client,
                         [&stderrBuffer](const QByteArray& chunk) { stderrBuffer += chunk; });

        QSignalSpy readySpy(&client, &McpClient::ready);
        QSignalSpy reconnectingSpy(&client, &McpClient::reconnecting);
        client.start();
        QVERIFY2(readySpy.wait(15000), "首次握手失败");

        // 第 1 次 ping 在 30s、第 2 次在 60s；给 75s 余量。
        QTRY_VERIFY_WITH_TIMEOUT(stderrBuffer.count("mock: ping") >= 2, 75000);
        QCOMPARE(reconnectingSpy.count(), 0); // 正常心跳不应触发重连

        client.shutdown();
    }
};

QTEST_MAIN(HeartbeatLongTest)
#include "tst_heartbeat_long.moc"
