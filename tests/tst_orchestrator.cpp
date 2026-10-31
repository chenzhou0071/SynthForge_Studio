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
    // 方案 A：unity 复用同一执行器时，必须用 Unity 说明书、跳过 Blender 专属的清场/导出
    void unityExecuteUsesUnityPromptAndSkipsBlenderSteps()
    {
        // unity 单组：build 轮（无工具）→ 自检轮（无工具）→ 直接收尾，不导出
        MockLlmServer server({textTurn(QStringLiteral("建了个立方体")),
                              textTurn(QStringLiteral("自检通过"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        BuildGroup group;
        group.title = QStringLiteral("Unity 任务");
        group.acceptance = QStringLiteral("read_console 无编译错误");
        plan.groups = {group};
        plan.maxSteps = 10;
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

        orch.execute(QStringLiteral("t1"), nullptr, QJsonArray{}, plan,
                     QStringLiteral("创建一个红色立方体"), QStringLiteral("Assets"),
                     QStringLiteral("unity"));
        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);

        // 系统提示词是 Unity 的，不能是 Blender 建模说明书
        QVERIFY2(!server.requestAt(0).contains(QStringLiteral("Blender 建模 agent")),
                 qPrintable(server.requestAt(0)));
        QVERIFY(server.requestAt(0).contains(QStringLiteral("Unity")));
        // 不能对 Unity 桥下 Blender 专属命令（清场 / 导出都靠 execute_blender_code）
        QVERIFY(!mcp.calls.contains(QStringLiteral("execute_blender_code")));
    }

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
        QVERIFY(server.requestAt(0).contains(QStringLiteral("不要自行导出"))); // 中途导出会撞名生成 _2
        QVERIFY(server.requestAt(0).contains(QStringLiteral("底座边长按真实尺度反推"))); // 不能把主体做小
        QVERIFY(server.requestAt(0).contains(QStringLiteral("压垮全场"))); // 体量配平，别建两座塔
        QVERIFY(server.requestAt(0).contains(QStringLiteral("自发光面片"))); // 窗户招牌禁悬空自发光片
        QVERIFY(server.requestAt(0).contains(QStringLiteral("歪斜的深色平面"))); // 路/水别糊一张歪平面
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
        QVERIFY(server.requestAt(2).contains(QStringLiteral("盒式舞台自检"))); // 自检措辞按档位读表
        QVERIFY(server.requestAt(2).contains(QStringLiteral("截图 1 次")));    // 不反复体检
        QVERIFY(server.requestAt(2).contains(QStringLiteral("只体检一次")));   // 一趟，不是两轮
        QVERIFY(!server.requestAt(2).contains(QStringLiteral("自检最多两轮"))); // 盒式舞台没被放宽
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

    // 建造/自检阶段自行 export/save 会被拦下，只有系统最后的导出回合能落盘
    void executorBlocksSelfExportBeforeExportRound()
    {
        MockLlmServer server(
            {MockLlmServer::Turn{
                 .frames = {MockLlm::toolCallChunk(
                                QStringLiteral("execute_blender_code"),
                                QStringLiteral(R"json({"code":"bpy.ops.export_scene.gltf(filepath='x.glb')"})json")),
                            MockLlm::doneChunk()}},
             textTurn(QStringLiteral("本组建造完成")),
             textTurn(QStringLiteral("自检通过")),
             MockLlmServer::Turn{
                 .frames = {MockLlm::toolCallChunk(
                                QStringLiteral("execute_blender_code"),
                                QStringLiteral(R"json({"code":"bpy.ops.export_scene.gltf(filepath='y.glb')"})json")),
                            MockLlm::doneChunk()}},
             textTurn(QStringLiteral("已导出"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());
        BuildPlan plan;
        BuildGroup group;
        group.title = QStringLiteral("底座与环境");
        group.items = {QStringLiteral("底座")};
        group.acceptance = QStringLiteral("俯视截图检查底座完整");
        plan.groups = {group};
        plan.maxSteps = 24;
        QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);
        QSignalSpy exportSpy(&orch, &AgentOrchestrator::exportStarted);

        orch.execute(QStringLiteral("t1"), nullptr, openAiTools(), plan,
                     QStringLiteral("做一个雨夜便利店"), QStringLiteral("/tmp/out"),
                     QStringLiteral("blender"));

        QTRY_COMPARE_WITH_TIMEOUT(finalSpy.count(), 1, 5000);
        QCOMPARE(server.requestCount(), 5); // 中途导出被拦后仍能继续，最后再走系统导出
        // 两次 MCP 调用 = 清场 + 系统导出；中途那次被拦下，没有落到 MCP
        QCOMPARE(mcp.calls.size(), 2);
        QCOMPARE(mcp.calls.first(), QStringLiteral("execute_blender_code"));
        QCOMPARE(mcp.calls.last(), QStringLiteral("execute_blender_code"));
        // 导出回合一开始就报给前端：预览面板先显示“导出中”，不加载半成品
        QCOMPARE(exportSpy.count(), 1);
        QCOMPARE(exportSpy.first().at(0).toString(), QStringLiteral("blender"));
        QCOMPARE(exportSpy.first().at(1).toString(), QStringLiteral("/tmp/out"));
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

    // 道具档：规划器不该再拿到「底座环境 → 建筑外观」这套施工顺序，也不该被框回方盒子
    void propPlanPromptHasNoSquareBaseplate()
    {
        MockLlmServer server({textTurn(QStringLiteral(
            R"({"groups":[{"title":"书桌主体","items":["桌面","桌腿"],)"
            R"("placements":[{"name":"桌面","where":"四腿顶端，离地 0.75 米"}],)"
            R"("acceptance":"正视截图：桌面水平、四腿落地"}]})"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t1"), nullptr, QStringLiteral("一张书桌"),
                  QStringLiteral("blender"), OutputShape::Prop);

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QVERIFY(!server.requestAt(0).contains(QStringLiteral("正方形")));
        // 道具档的规划器不该再要求「底座环境 → 建筑外观」这类施工顺序
        QVERIFY(server.requestAt(0).contains(QStringLiteral("主体 → 附属小件")));
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        QCOMPARE(plan.value(QStringLiteral("shape")).toString(), QStringLiteral("prop"));
        // 1 组道具不需要 120 步：预算按档位走
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 20);
    }

    // 开放场景：主轴分区与动线要能进计划卡，预算按 40/组
    void openPlanCarriesZonesAndBiggerBudget()
    {
        MockLlmServer server({textTurn(QStringLiteral(
            R"({"rules":["轨道沿 +Y 延伸不中断"],)"
            R"("zones":["主轴：轨道沿 +Y 至 120 米","近景：0~20 米 站台","远景：80 米外 隧道口"],)"
            R"("motions":["火车 0~120 米进站"],)"
            R"("groups":[{"title":"地面与主轴","items":["站台","轨道"],)"
            R"("placements":[{"name":"轨道","where":"站台东侧，沿 +Y 延伸"}],)"
            R"("acceptance":"沿主轴截图：轨道贯通不断裂"}]})"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t2"), nullptr, QStringLiteral("绿皮火车站"),
                  QStringLiteral("blender"), OutputShape::OpenScene);

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        QCOMPARE(plan.value(QStringLiteral("shape")).toString(), QStringLiteral("open"));
        QCOMPARE(plan.value(QStringLiteral("zones")).toArray().size(), 3);
        QCOMPARE(plan.value(QStringLiteral("motions")).toArray().size(), 1);
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 40); // 40/组
    }

    // 没有主轴与分区的「开放场景」就是一堆撒开的积木：必须重试，还不行才降级
    void openPlanWithoutZonesRetriesOnceThenFallsBack()
    {
        MockLlmServer server({
            textTurn(QStringLiteral(
                R"({"groups":[{"title":"一堆积木","items":["楼"],"placements":[)"
                R"({"name":"楼","where":"随便"}],"acceptance":"截图看看"}]})")),
            textTurn(QStringLiteral(
                R"({"groups":[{"title":"一堆积木","items":["楼"],"placements":[)"
                R"({"name":"楼","where":"随便"}],"acceptance":"截图看看"}]})")),
        });
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t3"), nullptr, QStringLiteral("一个公园"),
                  QStringLiteral("blender"), OutputShape::OpenScene);

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QCOMPARE(server.requestCount(), 2); // 缺 zones 触发一次重试
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        // 仍缺就降级到该档默认分组：默认分组自带主轴与三段分区，不会产出无方向布局
        QCOMPARE(plan.value(QStringLiteral("shape")).toString(), QStringLiteral("open"));
        QVERIFY2(plan.value(QStringLiteral("zones")).toArray().size() >= 2,
                 "降级分组没带分区，开放场景会变成无方向布局");
    }

    // 盒式舞台的降级路径一个行为都不许变：四组、120 步，只多一个 shape 字段
    void dioramaPlanDefaultsKeepWorkingWhenLlmReturnsJunk()
    {
        MockLlmServer server({textTurn(QStringLiteral("我不会输出 JSON")),
                              textTurn(QStringLiteral("我还是不会"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        AgentOrchestrator orch(&llm, nullptr, nullptr);
        orch.setModelConfig(server.config());
        QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

        orch.plan(QStringLiteral("t4"), nullptr, QStringLiteral("雨夜便利店"),
                  QStringLiteral("blender"), OutputShape::Diorama);

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QJsonObject plan = ready.first().at(2).toJsonObject();
        QCOMPARE(plan.value(QStringLiteral("groups")).toArray().size(), 4); // 现状四组
        QCOMPARE(plan.value(QStringLiteral("totalSteps")).toInt(), 120);
        QCOMPARE(plan.value(QStringLiteral("shape")).toString(), QStringLiteral("diorama"));
    }

    // 重启后 m_pendingPlans 丢失，历史 plan 行的 JSON 还在：旧 JSON 没有 shape 字段必须回落
    void buildPlanFromJsonDefaultsToDioramaForLegacyJson()
    {
        const BuildPlan plan = buildPlanFromJson(QJsonObject{
            {"groups",
             QJsonArray{QJsonObject{{"title", QStringLiteral("底座环境")},
                                    {"acceptance", QStringLiteral("俯视")}}}}});
        QVERIFY(plan.shape == OutputShape::Diorama);
        QCOMPARE(plan.maxSteps, 30); // 1 组 × 30：盒式舞台档单组预算
        QVERIFY(plan.zones.isEmpty());
        QVERIFY(plan.motions.isEmpty());
    }

    // 道具档：通用 11 条里「场景中心在原点、主建筑朝 -Y、底座边长反推」是微缩舞台专属，
    // 留在道具档就会把书桌拽回一个方框里
    void propSystemPromptDropsDioramaLayoutRules()
    {
        MockLlmServer server({textTurn(QStringLiteral("道具建好了")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("导出完成"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());

        BuildPlan plan;
        plan.shape = OutputShape::Prop;
        plan.groups = {{QStringLiteral("主体"), {QStringLiteral("书桌")}, {},
                        QStringLiteral("正视截图：桌面水平")}};
        plan.maxSteps = shapeBudget(OutputShape::Prop, 1);
        orch.execute(QStringLiteral("b1"), nullptr, openAiTools(), plan,
                     QStringLiteral("一张书桌"), QStringLiteral("Props"),
                     QStringLiteral("blender"));

        // 清场走 MCP 直调、不产生 LLM 请求：第一个 LLM 请求就是第 1 组的建造轮
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 1, 5000);
        const QString system = server.requestAt(0);
        QVERIFY2(system.contains(QStringLiteral("不要任何底座、地面、地平面对象")),
                 "道具档纪律没接上");
        QVERIFY(!system.contains(QStringLiteral("底座边长按真实尺度反推")));
        QVERIFY(!system.contains(QStringLiteral("主建筑正面朝 -Y")));
        // 通用纪律不能因为分档丢掉：Unity 导入依赖这三条
        QVERIFY2(system.contains(QStringLiteral("1 Blender 单位 = 1 米")), "丢了单位约定");
        QVERIFY2(system.contains(QStringLiteral("pack_all")), "丢了贴图打包约定");
        QVERIFY(system.contains(QStringLiteral("本档产出形态：道具")));
    }

    // 开放场景：主轴分区与动线必须落到每一组的指令里，不然模型只能猜
    void openGroupTurnInjectsZonesAndMotions()
    {
        MockLlmServer server({textTurn(QStringLiteral("主轴建好了")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("导出完成"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());

        BuildPlan plan;
        plan.shape = OutputShape::OpenScene;
        plan.zones = {QStringLiteral("主轴：轨道沿 +Y 延伸至 120 米"),
                      QStringLiteral("近景：0~20 米 站台")};
        plan.motions = {QStringLiteral("火车从 +Y 端驶入站台停下")};
        plan.groups = {{QStringLiteral("地面与主轴"), {QStringLiteral("轨道")},
                        {{QStringLiteral("轨道"), QStringLiteral("沿 +Y 延伸")}},
                        QStringLiteral("沿主轴截图：轨道贯通")}};
        plan.maxSteps = shapeBudget(OutputShape::OpenScene, 1);
        orch.execute(QStringLiteral("b2"), nullptr, openAiTools(), plan,
                     QStringLiteral("绿皮火车站"), QStringLiteral("Scenes"),
                     QStringLiteral("blender"));

        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 1, 5000);
        const QString userTurn = server.requestAt(0);
        QVERIFY2(userTurn.contains(QStringLiteral("主轴：轨道沿 +Y 延伸至 120 米")),
                 "分区没注入本组指令");
        QVERIFY2(userTurn.contains(QStringLiteral("火车从 +Y 端驶入站台停下")),
                 "动线没注入本组指令");
    }

    // 时序动画只截一张静图，模型永远发现不了火车穿模或原地不动
    void openSelfCheckAsksForFrameSetShots()
    {
        MockLlmServer server({textTurn(QStringLiteral("本组建好了")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("导出完成"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());

        BuildPlan plan;
        plan.shape = OutputShape::OpenScene;
        plan.groups = {{QStringLiteral("近景分区"), {QStringLiteral("站台")}, {},
                        QStringLiteral("近景截图：站台完整")}};
        plan.maxSteps = shapeBudget(OutputShape::OpenScene, 1);
        orch.execute(QStringLiteral("b3"), nullptr, openAiTools(), plan,
                     QStringLiteral("火车站"), QStringLiteral("Scenes"),
                     QStringLiteral("blender"));

        // 建造轮 = 请求 0，自检轮 = 请求 1
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 2, 5000);
        const QString selfCheck = server.requestAt(1);
        QVERIFY2(selfCheck.contains(QStringLiteral("frame_set")), "自检没要求按帧截图");
        QVERIFY2(selfCheck.contains(QStringLiteral("起始帧")), "自检没给出取哪几帧");
        // 档位没读错：盒式舞台那句自检不能套到动画档
        QVERIFY(!selfCheck.contains(QStringLiteral("盒式舞台自检")));
    }

    // 氛围层要在 Blender 里看得见、在 Unity 里不碍事：靠集合级过滤，不靠模型自觉
    void exportScriptExcludesFxCollectionFromGlbAndFbx()
    {
        MockLlmServer server({textTurn(QStringLiteral("建好了")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("导出完成"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());

        BuildPlan plan;
        plan.shape = OutputShape::Diorama;
        plan.groups = {{QStringLiteral("底座环境"), {}, {}, QStringLiteral("俯视截图")}};
        plan.maxSteps = shapeBudget(OutputShape::Diorama, 1);
        orch.execute(QStringLiteral("b4"), nullptr, openAiTools(), plan,
                     QStringLiteral("便利店"), QStringLiteral("Shop"),
                     QStringLiteral("blender"));

        // 建造轮 0 → 自检轮 1 → 导出轮 2（脚本轮都只出文字、不调工具）
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 3, 5000);
        const QString exportTurn = server.requestAt(2);
        QVERIFY2(exportTurn.contains(QStringLiteral("coll.name == 'FX'")),
                 "导出脚本没识别 FX 集合");
        QVERIFY2(exportTurn.contains(QStringLiteral("use_selection=True")),
                 "glb/fbx 还在全量导出，FX 会跟着进 Unity");
        // .blend 必须还带 FX：剔掉了就没有人在 Blender 里看效果的那份文件了
        QVERIFY2(exportTurn.contains(QStringLiteral("bpy.ops.file.pack_all")),
                 "丢了贴图打包");
        QVERIFY2(exportTurn.contains(QStringLiteral("save_as_mainfile")), "丢了 .blend 保存");
        // 全量保存不能被 use_selection 带跑：save_as_mainfile 没有选集过滤这回事
        QVERIFY(!exportTurn.contains(QStringLiteral("save_as_mainfile(filepath=os.path.join("
                                                    "folder, base + \".blend\"), use_selection")));
        // 舞台档要告诉模型 FX 是被有意排除的，否则它会改名塞回 GLB
        QVERIFY2(exportTurn.contains(QStringLiteral("已被有意排除")), "没交代 FX 的去向");
    }

    // 道具进 Unity 要能整体拖、也能拆开单用：根空对象必须留在导出选择集里
    void propExportKeepsRootEmptyAndNoBaseplate()
    {
        MockLlmServer server({textTurn(QStringLiteral("建好了")),
                              textTurn(QStringLiteral("自检通过")),
                              textTurn(QStringLiteral("导出完成"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
        orch.setModelConfig(server.config());

        BuildPlan plan;
        plan.shape = OutputShape::Prop;
        plan.groups = {{QStringLiteral("主体"), {}, {}, QStringLiteral("正视截图")}};
        plan.maxSteps = shapeBudget(OutputShape::Prop, 1);
        orch.execute(QStringLiteral("b5"), nullptr, openAiTools(), plan,
                     QStringLiteral("书桌"), QStringLiteral("Props"),
                     QStringLiteral("blender"));

        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 3, 5000);
        const QString exportTurn = server.requestAt(2);
        QVERIFY2(exportTurn.contains(QStringLiteral("根空对象保持选中")),
                 "道具档没保住根空对象（层级会散）");
        QVERIFY2(exportTurn.contains(QStringLiteral("先删掉再导出")),
                 "道具档没提醒清掉误长的底座");
        // 道具没有氛围层，那句舞台档的 FX 交代不该出现
        QVERIFY(!exportTurn.contains(QStringLiteral("已被有意排除")));
    }
};

QTEST_MAIN(OrchestratorTest)
#include "tst_orchestrator.moc"
