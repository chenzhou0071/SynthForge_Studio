#include <QtTest>

#include "core/mcp/BridgeCommands.h"

class BridgeCommandsTest : public QObject
{
    Q_OBJECT

private slots:
    void blenderDefaults()
    {
        const BridgeCommand cmd = defaultBridgeCommand(QStringLiteral("blender"));
        QCOMPARE(cmd.program, QStringLiteral("uvx"));
        QCOMPARE(cmd.args, (QStringList{QStringLiteral("--python"), QStringLiteral("3.11"),
                                        QStringLiteral("mcp-for-blender")}));
        QCOMPARE(cmd.env.value(QStringLiteral("UV_PYTHON_PREFERENCE")),
                 QStringLiteral("only-managed"));
    }

    void unityDefaults()
    {
        const BridgeCommand cmd = defaultBridgeCommand(QStringLiteral("unity"));
        QCOMPARE(cmd.program, QStringLiteral("uvx"));
        QCOMPARE(cmd.args,
                 (QStringList{QStringLiteral("--from"), QStringLiteral("mcpforunityserver"),
                              QStringLiteral("mcp-for-unity"), QStringLiteral("--transport"),
                              QStringLiteral("stdio")}));
    }

    void unknownAreaReturnsEmptyProgram()
    {
        QVERIFY(defaultBridgeCommand(QStringLiteral("photoshop")).program.isEmpty());
    }
};

QTEST_MAIN(BridgeCommandsTest)
#include "tst_bridge_commands.moc"
