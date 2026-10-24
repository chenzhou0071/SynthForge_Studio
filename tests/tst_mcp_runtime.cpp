#include <QJsonArray>
#include <QProcessEnvironment>
#include <QtTest>

#include <memory>

#include "core/mcp/McpRuntime.h"
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

class McpRuntimeTest : public QObject
{
    Q_OBJECT
private slots:
    void handshakeCachesToolsAndRestarts()
    {
        BridgeCommand command{mockServerProgram(), {},
                              QProcessEnvironment::systemEnvironment()};
        McpRuntime runtime(QStringLiteral("blender"),
                           std::make_unique<StdioTransport>(command));
        QSignalSpy statusSpy(&runtime, &McpRuntime::statusChanged);
        QSignalSpy toolsSpy(&runtime, &McpRuntime::toolsChanged);

        QVERIFY(runtime.start());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.tools().size() == 1, 15000);
        QCOMPARE(toolsSpy.count(), 1);

        QVERIFY(runtime.restart());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(toolsSpy.count() >= 2, 15000);

        int readyCount = 0;
        for (const auto& args : statusSpy)
            if (args.at(1).toString() == QStringLiteral("ready"))
                ++readyCount;
        QVERIFY2(readyCount >= 2, "首次握手与重启后都应广播 ready");
    }
};

QTEST_MAIN(McpRuntimeTest)
#include "tst_mcp_runtime.moc"
