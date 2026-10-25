#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "core/agent/AgentOrchestrator.h"
#include "core/llm/LlmGateway.h"
#include "core/mcp/BlenderGate.h"
#include "core/tasks/TaskManager.h"
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

// 先说话再干活：同一轮里既有交代文字，又发起工具调用
MockLlmServer::Turn textThenToolTurn(const QString& text, const QString& name)
{
    return MockLlmServer::Turn{.frames = {MockLlm::textChunk(text), MockLlm::toolCallChunk(name),
                                          MockLlm::doneChunk()}};
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
    void plannerParsesGroupsAndLayout()
    {
        MockLlmServer server({textTurn(QStringLiteral(
            R"({"rules":["树不遮挡招牌"],"groups":[)"
            R"({"title":"底座与环境","items":["底座","路牌"],)"
            R"("placements":[{"name":"底座","where":"场景中心，沿 X 轴展开"},)"
            R"({"name":"路牌","where":"商店右前方 2 米，正面朝 -Y"}],)"
            R"("acceptance":"俯视截图：底座完整"},)"
            R"({"title":"建筑外观","items":["墙体","招牌"],)"
            R"("placements":[{"name":"墙体","where":"底座北侧边缘"},)"
            R"({"name":"招牌","where":"正门上方，离地 2.8 米"}],)"
            R"("acceptance":"街角截图：主体齐全"}]})"))});
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
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 60); // 30*2
        QVERIFY(server.requestAt(0).contains(QStringLiteral("合理补充"))); // 主题内可以发散
        QVERIFY(server.requestAt(0).contains(QStringLiteral("与主题无关"))); // 只禁无关物件
        QCOMPARE(plan.value(QStringLiteral("rules")).toArray().size(), 1);
        const QJsonArray placements = plan.value(QStringLiteral("groups"))
                                          .toArray()
                                          .at(0)
                                          .toObject()
                                          .value(QStringLiteral("placements"))
                                          .toArray();
        QCOMPARE(placements.size(), 2);
        QCOMPARE(placements.at(1).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("路牌"));
        QVERIFY(placements.at(1).toObject().value(QStringLiteral("where")).toString().contains(
            QStringLiteral("右前方")));
    }

    // plan.confirm 收到的是 planReady 的 JSON，必须原样还原成 BuildPlan（含布局）
    void planJsonRestoresBuildPlan()
    {
        const QJsonObject json{
            {"totalSteps", 26},
            {"rules", QJsonArray{QStringLiteral("树不遮挡招牌")}},
            {"groups",
             QJsonArray{
                 QJsonObject{{"title", QStringLiteral("底座与环境")},
                             {"items", QJsonArray{QStringLiteral("底座"), QStringLiteral("路牌")}},
                             {"placements",
                              QJsonArray{QJsonObject{{"name", QStringLiteral("路牌")},
                                                     {"where", QStringLiteral("商店右前方 2 米")}}}},
                             {"acceptance", QStringLiteral("俯视截图：底座完整")}},
                 QJsonObject{{"title", QStringLiteral("建筑外观")},
                             {"items", QJsonArray{QStringLiteral("招牌")}},
                             {"placements", QJsonArray{}},
                             {"acceptance", QStringLiteral("街角截图：主体齐全")}}}},
        };

        const BuildPlan plan = buildPlanFromJson(json);

        QCOMPARE(plan.maxSteps, 26);
        QCOMPARE(plan.rules.size(), 1);
        QCOMPARE(plan.groups.size(), 2);
        QCOMPARE(plan.groups.at(0).placements.size(), 1);
        QCOMPARE(plan.groups.at(0).placements.at(0).name, QStringLiteral("路牌"));
        QVERIFY(plan.groups.at(0).placements.at(0).where.contains(QStringLiteral("右前方")));
        QCOMPARE(plan.groups.at(1).items.first(), QStringLiteral("招牌"));
        QCOMPARE(plan.groups.at(1).acceptance, QStringLiteral("街角截图：主体齐全"));
    }

    // 预算按组数线性给，封顶 200：2/4/6 组分别 60/120/180，7 组以上不再涨
    void planBudgetScalesWithGroupCount()
    {
        const auto planWith = [](int groupCount) {
            QJsonArray groups;
            for (int i = 0; i < groupCount; ++i)
                groups.append(QJsonObject{{"title", QStringLiteral("组%1").arg(i + 1)}});
            return buildPlanFromJson(QJsonObject{{"groups", groups}});
        };

        QCOMPARE(planWith(2).maxSteps, 60);
        QCOMPARE(planWith(4).maxSteps, 120);
        QCOMPARE(planWith(6).maxSteps, 180);
        QCOMPARE(planWith(7).maxSteps, 200);
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
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 120); // 30*4
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
        BuildGroup group;
        group.title = QStringLiteral("底座与环境");
        group.items = {QStringLiteral("底座")};
        group.placements = {{QStringLiteral("底座"), QStringLiteral("场景中心，沿 X 轴展开")}};
        group.acceptance = QStringLiteral("俯视截图检查底座完整");
        plan.groups = {group};
        plan.rules = {QStringLiteral("树不遮挡招牌")};
        plan.maxSteps = 18;
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan,
                     QStringLiteral("做一个雨夜便利店"), QStringLiteral("/tmp/out"),
                     QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);
        QCOMPARE(server.requestCount(), 4); // 建造 → 自检 → 导出 → 收尾
        QVERIFY(server.requestAt(0).contains(QStringLiteral("简体中文"))); // 面向用户的话必须是中文
        QVERIFY(server.requestAt(0).contains(QStringLiteral("底座边长按真实尺度反推"))); // 不能把主体做小
        QVERIFY(server.requestAt(0).contains(QStringLiteral("与主题无关"))); // 合理补充也有限度
        QVERIFY(server.requestAt(0).contains(QStringLiteral("真实光源"))); // 灯光必须是真实光源
        QVERIFY(server.requestAt(0).contains(QStringLiteral("飞虫"))); // 不默认生成飞虫/湿地面
        QVERIFY(server.requestAt(0).contains(QStringLiteral("光粒子"))); // 光粒子等特效可以保留
        QVERIFY(server.requestAt(0).contains(QStringLiteral("沿 X 轴展开"))); // 组内摆放已注入
        QVERIFY(server.requestAt(0).contains(QStringLiteral("不要复述截图里的细节"))); // 截图后只说结论
        QVERIFY(server.requestAt(0).contains(QStringLiteral("中间轮次不要写验收报告"))); // 组结束别写小作文
        QVERIFY(server.requestAt(0).contains(QStringLiteral("只回 1 句结果"))); // 组完成只报结果
        QVERIFY(server.requestAt(2).contains(QStringLiteral("验收标准")));    // 自检指令已注入
        QVERIFY(server.requestAt(2).contains(QStringLiteral("树不遮挡招牌"))); // 任务约束注入自检
        QVERIFY(server.requestAt(2).contains(QStringLiteral("只做一次自检"))); // 不反复体检
        QVERIFY(server.requestAt(2).contains(QStringLiteral("不要逐项复述"))); // 不复述坐标间隙
        QVERIFY(server.requestAt(2).contains(QStringLiteral("不要复述本条要求"))); // 别把提示词抄一遍
        QVERIFY(server.requestAt(3).contains(QStringLiteral("export_scene"))); // 导出指令已注入
        QVERIFY(server.requestAt(3).contains(QStringLiteral("不要再做场景体检"))); // 导出前不再重复校验
        QVERIFY(server.requestAt(3).contains(QStringLiteral("save_version = 0"))); // 不生成 .blend1 备份
        QVERIFY(server.requestAt(3).contains(QStringLiteral("只执行一次"))); // 避免换名重复导出
        QVERIFY(server.requestAt(3).contains(QStringLiteral("不要复述本条要求或脚本内容"))); // 别复述导出指令
        QVERIFY(server.requestAt(3).contains(QStringLiteral("最终回复保持简短"))); // 收尾别写小作文
        QVERIFY(server.requestAt(3).contains(QStringLiteral("最多 5 条清单")));   // 清单保留
    }

    // 修改已有资产：导出必须覆盖同名文件，不能再走“撞名加序号”
    void executorModifyOverwritesExistingBase()
    {
        MockLlmServer server({toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("已改短，动画保留")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("已覆盖导出"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        BuildGroup group;
        group.title = QStringLiteral("修改资产");
        group.acceptance = QStringLiteral("剑更短且动画仍在");
        plan.groups = {group};
        plan.maxSteps = 20;
        plan.targetBase = QStringLiteral("sword_01");
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t-modify"), nullptr, openAiTools(), plan,
                     QStringLiteral("打开 .blend 源文件 /tmp/out/sword_01.blend。要求：把剑改短一点。"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);
        const QString exportRequest = server.requestAt(3);
        QVERIFY(exportRequest.contains(QStringLiteral("sword_01"))); // 基名来自目标资产
        QVERIFY(exportRequest.contains(QStringLiteral("覆盖")));     // 明确覆盖同名文件
        QVERIFY(!exportRequest.contains(QStringLiteral("while os.path.exists"))); // 不再加序号
        QVERIFY(exportRequest.contains(QStringLiteral("最终回复保持简短"))); // 收尾别写小作文
        QVERIFY(exportRequest.contains(QStringLiteral("最多 5 条清单")));   // 清单保留
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
        plan.groups = {{QStringLiteral("底座与环境"), {}, {}, QStringLiteral("检查底座")},
                       {QStringLiteral("建筑外观"), {}, {}, QStringLiteral("检查建筑")}};
        QSignalSpy progress(&orch, &AgentOrchestrator::groupProgress);
        QSignalSpy turns(&orch, &AgentOrchestrator::turnFinished);
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 8000);
        QCOMPARE(progress.count(), 2);             // 两组各一次进度
        QCOMPARE(progress.at(1).at(1).toInt(), 2); // 第二组 index
        // 每轮文字在进入下一步前收口：第 2 组的话不能续在第 1 组气泡里
        QCOMPARE(turns.count(), 4);
        QCOMPARE(turns.at(0).at(1).toString(), QStringLiteral("组1建造完成"));
        QCOMPARE(turns.at(1).at(1).toString(), QStringLiteral("组1自检通过"));
        QCOMPARE(turns.at(2).at(1).toString(), QStringLiteral("组2建造完成"));
        QCOMPARE(turns.at(3).at(1).toString(), QStringLiteral("组2自检通过"));
    }

    // 工具调用前的交代也要单独成气泡：先说话 → 再干活 → 看截图 → 再说下一段
    void turnFinishedSplitsEachAssistantTurn()
    {
        MockLlmServer server({textThenToolTurn(QStringLiteral("先检查场景"),
                                               QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("组1建造完成")),
                              textTurn(QStringLiteral("组1自检通过")),
                              textTurn(QStringLiteral("已导出 glb/fbx/blend"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"), {}, {}, QStringLiteral("检查底座")}};
        plan.maxSteps = 18;
        QSignalSpy turns(&orch, &AgentOrchestrator::turnFinished);
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 8000);
        QCOMPARE(turns.count(), 3);
        QCOMPARE(turns.at(0).at(1).toString(), QStringLiteral("先检查场景"));
        QCOMPARE(turns.at(1).at(1).toString(), QStringLiteral("组1建造完成"));
        QCOMPARE(turns.at(2).at(1).toString(), QStringLiteral("组1自检通过"));
    }

    // 停止修改任务必须断掉在途流：只打 cancelled 标记，卡在 stall 的请求会一直占着 UI
    void cancelAbortsInFlightStream()
    {
        MockLlmServer server({MockLlmServer::Turn{
            .frames = {MockLlm::textChunk(QStringLiteral("正在修改…"))},
            .stallAfterFrames = true}});
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        TaskManager tm;
        BuildPlan plan;
        plan.groups = {{QStringLiteral("修改资产"), {}, {}, QStringLiteral("改完")}};
        QSignalSpy finished(&tm, &TaskManager::finished);

        tm.enqueue(Task{QStringLiteral("t1"), QStringLiteral("blender"), QString(),
                        [&orch, &plan](const QString& tid, TaskManager* manager) {
                            orch.execute(tid, manager, openAiTools(), plan, QStringLiteral("p"),
                                         QStringLiteral("/tmp/out"), QStringLiteral("blender"));
                        }});
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 3000);

        // 与 CoreApp::chat.cancel 一致：先标记，再按区断网
        tm.cancelArea(QStringLiteral("blender"));
        llm.abortTag(QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(finished.first().at(1).toBool(), true);
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
        plan.groups = {{QStringLiteral("底座与环境"), {}, {}, QStringLiteral("检查底座")}};
        QSignalSpy failed(&orch, &AgentOrchestrator::failed);

        orch.execute(QStringLiteral("t1"), nullptr, {}, plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("blender-busy"));
        QCOMPARE(server.requestCount(), 0);                      // 门禁失败不碰 LLM / MCP
        QCOMPARE(gate.owner(), QStringLiteral("unity-convert")); // 不释放他人租约
    }

    // 步数撞上限不该把整场景判死：保留会话与分组位置，用户点「继续工作」接着跑
    void executorResumesAfterMaxSteps()
    {
        MockLlmServer server({toolTurn(QStringLiteral("execute_blender_code")),
                              textTurn(QStringLiteral("组1建造完成")),
                              textTurn(QStringLiteral("组1自检通过")),
                              textTurn(QStringLiteral("已导出 glb/fbx/blend"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BlenderGate gate;
        orch.setBlenderGate(&gate);
        BuildPlan plan;
        plan.groups = {{QStringLiteral("底座与环境"), {}, {}, QStringLiteral("检查底座")}};
        plan.maxSteps = 1;
        QVERIFY(!orch.canResume(QStringLiteral("blender"))); // 没跑过不能续
        QSignalSpy failed(&orch, &AgentOrchestrator::failed);
        QSignalSpy progress(&orch, &AgentOrchestrator::groupProgress);
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan, QStringLiteral("p"),
                     QStringLiteral("/tmp/out"), QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("max-steps-exceeded"));
        QVERIFY(orch.canResume(QStringLiteral("blender")));
        QCOMPARE(orch.stepCount(), 1); // 撞上限的那一步没发生，不记在用户头上
        QVERIFY(!gate.isBusy());       // 失败必须让出 Blender

        orch.resume(QStringLiteral("t2"), nullptr);

        QCOMPARE(orch.stepBudget(), 51); // 1 + min(30*1+20, 200)：续跑追加预算
        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 8000);
        QVERIFY(server.requestAt(1).contains(QStringLiteral("\"role\":\"tool\""))); // 原会话还在
        QCOMPARE(progress.count(), 1); // 续跑不重复插入分组分隔条
        QVERIFY(!orch.canResume(QStringLiteral("blender")));
        QVERIFY(!gate.isBusy());
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
        plan.groups = {{QStringLiteral("底座与环境"), {}, {}, QStringLiteral("检查底座")}};
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
