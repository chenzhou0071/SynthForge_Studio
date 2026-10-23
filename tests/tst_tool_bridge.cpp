#include <QtTest>

#include "core/llm/ToolBridge.h"

class ToolBridgeTest : public QObject
{
    Q_OBJECT

private slots:
    void mcpToolsBecomeOpenAiFunctions()
    {
        const QJsonArray mcp{
            QJsonObject{
                {"name", "execute_blender_code"},
                {"description", "执行 Blender Python 代码"},
                {"inputSchema", QJsonObject{
                    {"type", "object"},
                    {"properties", QJsonObject{
                        {"code", QJsonObject{{"type", "string"}}}
                    }}
                }}
            }
        };

        const QJsonArray tools = ToolBridge::mcpToolsToOpenAi(mcp);

        QCOMPARE(tools.size(), 1);
        const QJsonObject tool = tools.first().toObject();
        QCOMPARE(tool.value("type").toString(), QStringLiteral("function"));
        const QJsonObject fn = tool.value("function").toObject();
        QCOMPARE(fn.value("name").toString(), QStringLiteral("execute_blender_code"));
        QVERIFY(fn.contains("parameters")); // inputSchema → parameters
        QVERIFY(!fn.contains("inputSchema"));
    }

    void internalCallsBecomeWireFormat()
    {
        const QJsonArray internal{
            QJsonObject{{"id", "call_1"},
                        {"name", "execute_blender_code"},
                        {"arguments", QJsonObject{{"code", "print(1)"}}}}};

        const QJsonArray wire = ToolBridge::internalCallsToWire(internal);

        QCOMPARE(wire.size(), 1);
        const QJsonObject call = wire.first().toObject();
        QCOMPARE(call.value("id").toString(), QStringLiteral("call_1"));
        QCOMPARE(call.value("type").toString(), QStringLiteral("function"));
        const QJsonObject fn = call.value("function").toObject();
        QCOMPARE(fn.value("name").toString(), QStringLiteral("execute_blender_code"));
        QCOMPARE(fn.value("arguments").toString(), QStringLiteral("{\"code\":\"print(1)\"}"));
    }
};

QTEST_MAIN(ToolBridgeTest)
#include "tst_tool_bridge.moc"
