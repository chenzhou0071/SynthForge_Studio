#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "core/agent/AgentOrchestrator.h"
#include "core/llm/LlmGateway.h"
#include "core/mcp/BlenderGate.h"
#include "mock/MockLlmServer.h"
#include "mock/MockMcpHarness.h"

namespace {

MockLlmServer::Turn textTurn(const QString& text)
{
    return MockLlmServer::Turn{.frames = {MockLlm::textChunk(text), MockLlm::doneChunk()}};
}

MockLlmServer::Turn toolTurn(const QString& name)
{
    return MockLlmServer::Turn{.frames = {MockLlm::toolCallChunk(name), MockLlm::doneChunk()}};
}

// 与 CoreApp 传 execute 的格式一致：OpenAI tools，而不是 MCP 原始 tools
QJsonArray openAiTools()
{
    const QJsonObject parameters{{"type", "object"},
                                 {"properties", QJsonObject{{"code", QJsonObject{{"type", "string"}}}}}};
    const QJsonObject function{{"name", "execute_blender_code"},
                               {"description", "执行 Blender Python 代码"},
                               {"parameters", parameters}};
    return QJsonArray{QJsonObject{{"type", "function"}, {"function", function}}};
}

} // namespace

// 集成测试：编排器走真实 LlmGateway（HTTP + SSE 由 MockLlmServer 提供假服务端），
// 覆盖 规划解析 / 降级重试 / 执行状态机 / 门禁互斥。
class OrchestratorTest : public QObject
{
    Q_OBJECT

private slots:
    void plannerParsesGroups()
    {
        MockLlmServer server({textTurn(QStringLiteral(
            R"({"groups":[{"title":"底座与环境","items":["底座","井盖"],"acceptance":"俯视截图：底座完整"},)"
            R"({"title":"建筑外观","items":["墙体","招牌"],"acceptance":"街角截图：主体齐全"}]})"))});
        QVERIFY(server.listen());
        LlmGateway llm; // 真实网关，禁止子类重写 chatStream
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t1"), nullptr, QStringLiteral("做一个雨夜便利店"),
                  QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        QCOMPARE(plan.value(QStringLiteral("groups")).toArray().size(), 2);
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 26); // 8*2+10
    }

    void plannerFallsBackToDefaults()
    {
        MockLlmServer server({textTurn(QStringLiteral("抱歉，我无法输出 JSON。")),
                              textTurn(QStringLiteral("仍然无法输出。"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t1"), nullptr, QStringLiteral("做一个雨夜便利店"),
                  QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        QCOMPARE(plan.value(QStringLiteral("groups")).toArray().size(), 4); // 默认四分组
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 42);
        QCOMPARE(server.requestCount(), 2); // 解析失败重试 1 次
    }

    void executorBuildThenSelfChecks()
    {
        MockLlmServer server({toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("本组建造完成")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("已导出 glb/fbx/blend"))});
        QVERIFY(server.listen());
        LlmGateway llm; // 真实网关，脚本走真实 HTTP+SSE
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"),
                        {QStringLiteral("底座")},
                        QStringLiteral("俯视截图检查底座完整")}};
        plan.maxSteps = 18;
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan,
                     QStringLiteral("做一个雨夜便利店"), QStringLiteral("/tmp/out"),
                     QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);
        QCOMPARE(server.requestCount(), 4); // 建造 → 自检 → 导出 → 收尾
        QVERIFY(server.requestAt(2).contains(QStringLiteral("验收标准")));   // 自检指令已注入
        QVERIFY(server.requestAt(3).contains(QStringLiteral("export_scene"))); // 导出指令已注入
    }

    void executorAdvancesGroups()
    {
        MockLlmServer server({toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("组1建造完成")),
                              textTurn(QStringLiteral("组1自检通过")),
                              toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("组2建造完成")),
                              textTurn(QStringLiteral("组2自检通过")),
                              textTurn(QStringLiteral("已导出 glb/fbx/blend"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"), {}, QStringLiteral("检查底座")},
                       {QStringLiteral("建筑外观"), {}, QStringLiteral("检查建筑")}};
        QSignalSpy progress(&orch, &AgentOrchestrator::groupProgress);
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 8000);
        QCOMPARE(progress.count(), 2);             // 两组各一次进度
        QCOMPARE(progress.at(1).at(1).toInt(), 2); // 第二组 index
    }

    void executorRefusesWhenBlenderGateBusy()
    {
        MockLlmServer server({}); // 不应发出任何请求
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        BlenderGate gate;
        QVERIFY(gate.tryAcquire(QStringLiteral("unity-convert")));
        orch.setBlenderGate(&gate);
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"), {}, QStringLiteral("检查底座")}};
        QSignalSpy failed(&orch, &AgentOrchestrator::failed);

        orch.execute(QStringLiteral("t1"), nullptr, {}, plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("blender-busy"));
        QCOMPARE(server.requestCount(), 0);                      // 门禁失败不碰 LLM / MCP
        QCOMPARE(gate.owner(), QStringLiteral("unity-convert")); // 不释放他人租约
    }

    void executorReleasesGateOnFinish()
    {
        MockLlmServer server({toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("本组建造完成")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("已导出 glb/fbx/blend"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BlenderGate gate;
        orch.setBlenderGate(&gate);
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"), {}, QStringLiteral("检查底座")}};
        plan.maxSteps = 18;
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);
        QVERIFY(!gate.isBusy()); // 成功出口必须释放
    }
};

QTEST_MAIN(OrchestratorTest)
#include "tst_orchestrator.moc"
