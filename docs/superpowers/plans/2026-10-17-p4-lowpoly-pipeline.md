# SynthForge Studio P4：低模建模全链实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** blender 区低模模式完整闭环：发送前置检查 → 总结 agent 生成专业提示词草稿（可编辑/发送）→ 规划调用生成建造计划卡（分组/清单/验收标准，可确认）→ 存放位置确认框 → 分组执行主循环（建造→自检，工具调用经 Blender MCP，截图回传对话区）→ 导出 GLB 落盘 → 文件树更新 → three.js 预览。

**Architecture:** `PreflightChecker` 在 chat.send / draft.send 入口做拒绝式检查；`SummarizerAgent` 用独立 system prompt 生成草稿并广播 `draft.created`；`AgentOrchestrator` 分两阶段——`plan()` 一次纯文本调用产出分组计划（PlanCard 确认），`execute()` 按组「建造 → 自检」状态机驱动 LLM tool_calls，工具调用经 P1 `McpClient`；截图压缩后以 base64 回喂模型并广播前端；落盘路径由 `folderPick` 确认后执行 `bpy.ops.export_scene.gltf`。

**规格映射：** 5.2 发送链路与总结 agent、5.3 建造计划确认、5.4 存放位置确认框、5.6 低模执行链（分组自检）、5.5 落盘与预览、4.4 预览面板、9 错误处理。

**依赖：** P2（LlmGateway / TaskManager / WS）、P3（聊天面板 / 文件树 / 预览骨架）。

## Global Constraints

- **前置检查先于总结 agent**：云模式未配置 / Blender 链未就绪 → 直接 `error.card`（`reason` 分别为 `cloud3d-not-configured` / `blender-not-ready`），不浪费改写调用。
- 总结 agent 与主 agent 配置相互独立；总结 agent 未配置时回退主 agent 配置。
- 工具清单按区裁剪：blender 区仅 Blender MCP 工具 + 文件工具。
- 截图回传模型前统一压缩：最长边 ≤1024px，JPEG q80，base64。
- 资产库根固定：`文档/SynthForgeStudio/ModelLibrary`（QStandardPaths::DocumentsLocation）；存放位置只能在该根的子路径中选择。
- 落盘后 GLB 与 .blend 一并保留；命名遵循草稿中的英文命名清单。
- **低模新资产走 Planner→Executor**：草稿发送后先规划、计划确认后才选存放位置并执行；解析失败降级默认四分组，不阻塞。修改已有资产不经过规划。
- 步数预算按计划动态计算：`maxSteps = min(8 × 组数 + 10, 60)`。

## File Structure

```
src/core/preflight/PreflightChecker.h/.cpp   ← Blender/云链就绪检查
src/core/agent/SummarizerAgent.h/.cpp        ← 草稿生成（draft.created/draft.send）
src/core/agent/AgentOrchestrator.h/.cpp      ← plan() 规划调用 + execute() 分组执行状态机 + 截图压缩 + 取消
src/core/assets/AssetLibrary.h/.cpp          ← 资产库根扫描/落盘/导出 GLB 编排
src/resources/prompts/summarizer.md          ← 总结 agent system prompt（骨架来自规格附录）
web/src/components/DraftCard.vue             ← 草稿卡（双击编辑 + 发送）
web/src/components/PlanCard.vue              ← 建造计划卡（分组/清单/验收标准 + 开始/取消）
web/src/components/ChatPanel.vue             ← Modify：draft/plan 消息卡 + 修改资产目标 chip（Task 6）
web/src/components/FolderPicker.vue          ← 存放位置确认弹窗
web/src/components/PreviewPane.vue           ← three.js GLB 预览（升级 Task 5）
tests/tst_preflight.cpp
tests/tst_orchestrator.cpp                   ← mock MCP + mock LLM 集成
```

---

### Task 1: PreflightChecker + chat.send 模式分支

**Files:** Create `src/core/preflight/PreflightChecker.h/.cpp`、`tests/tst_preflight.cpp`；Modify `src/core/coreapp/CoreApp.cpp`（chat.send 增加 `mode` 分支）。

**Interfaces:**
- `struct PreflightResult { bool ok; QString reason; }`
- `class PreflightChecker { explicit PreflightChecker(SettingsStore*, McpClient* blenderClient); PreflightResult checkLowpoly(); PreflightResult checkCloud3d(); }`
- `chat.send` 新参数：`{area, text, mode}`（`mode ∈ {"lowpoly","cloud3d"}`，缺省 `lowpoly`）。检查失败返回 `{"ok":false}` 并广播 `error.card {area, reason}`。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QtTest>
#include "core/preflight/PreflightChecker.h"

class PreflightTest : public QObject
{
    Q_OBJECT
private slots:
    void lowpolyFailsWithoutBlenderConnection()
    {
        SettingsStore settings(QDir::tempPath());   // 测试构造辅助：允许注入
        PreflightChecker checker(&settings, nullptr); // nullptr = 未连接
        const auto result = checker.checkLowpoly();
        QVERIFY(!result.ok);
        QCOMPARE(result.reason, QStringLiteral("blender-not-ready"));
    }

    void cloud3dFailsWithoutApiKey()
    {
        SettingsStore settings(QDir::tempPath());
        PreflightChecker checker(&settings, nullptr);
        const auto result = checker.checkCloud3d();
        QVERIFY(!result.ok);
        QCOMPARE(result.reason, QStringLiteral("cloud3d-not-configured"));
    }
};

QTEST_MAIN(PreflightTest)
#include "tst_preflight.moc"
```

- [ ] **Step 2: 实现**

```cpp
#pragma once

#include <QString>

class SettingsStore;
class McpClient;

struct PreflightResult
{
    bool ok = false;
    QString reason; // error.card 的 reason 字段
};

class PreflightChecker
{
public:
    // blenderClient 为 nullptr（桥接未连接）即视为链未就绪；
    // 连接状态由调用方用 client->isReady() 判定后传入。
    PreflightChecker(SettingsStore* settings, McpClient* blenderClient);

    PreflightResult checkLowpoly() const;   // Blender 链就绪
    PreflightResult checkCloud3d() const;   // 当前 3D 服务商必填项完整

private:
    SettingsStore* m_settings;
    McpClient* m_blenderClient;
};
```

`checkCloud3d` 判定：`cloud3d.provider` 为 `tripo` 需 `apiKey` 非空；`hunyuan` 需 `secretId`+`secretKey`；`custom` 需 `baseUrl` 非空；缺失返回 `{false, "cloud3d-not-configured"}`。

CoreApp 的 `chat.send` handler 在落库之前插入：

```cpp
const QString mode = params.value("mode").toString(QStringLiteral("lowpoly"));
const PreflightResult pre = mode == "cloud3d" ? m_preflight->checkCloud3d()
                                              : m_preflight->checkLowpoly();
if (!pre.ok) {
    m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", pre.reason}});
    return QJsonValue(QJsonObject{{"ok", false}});
}
// lowpoly：进入 Task 2 的总结 agent；cloud3d：P5 前暂回 {"ok":false} 占位错误
```

- [ ] **Step 3: CMake + 测试**（`tst_preflight`，2 用例；`SettingsStore` 测试辅助：直接构造临时目录实例并 load）

- [ ] **Step 4: Commit**（`feat: 发送前置检查（Blender 链 / 云配置）`）

---

### Task 2: SummarizerAgent（草稿流）

**Files:** Create `src/core/agent/SummarizerAgent.h/.cpp`、`src/resources/prompts/summarizer.md`（内容以规格附录参考提示词为骨架，把「雨夜便利店」替换为 `{{user_request}}` 与 `{{mode}}` 模板变量）；Modify `CoreApp`（`chat.send` lowpoly 分支 → 调用；新增 `draft.send`、`draft.save` handler）。

**Interfaces:**
- `class SummarizerAgent : QObject { void generate(area, userText); signals: void draftReady(area, QString prompt); }`
- WS 交互：完成后广播 `draft.created {area, prompt}`；前端编辑后发 `draft.save {area, prompt}`（落 SessionStore.drafts）；`draft.send {area, prompt}` → 进入 Task 3 规划阶段（生成建造计划卡）；`plan.confirm` 后才走 folderPick（Task 4），再由 `execute()` 执行。

- [ ] **Step 1: 实现**

```cpp
void SummarizerAgent::generate(const QString& area, const QString& userText)
{
    const QJsonObject agentCfg = resolveConfig(); // summarizer 未配置 → 回退 primary
    QFile tpl(":/prompts/summarizer.md");
    tpl.open(QIODevice::ReadOnly);
    QString system = QString::fromUtf8(tpl.readAll());
    system.replace("{{user_request}}", userText);

    QJsonArray messages{QJsonObject{{"role", "system"}, {"content", system}},
                        QJsonObject{{"role", "user"}, {"content", userText}}};
    LlmConfig cfg{agentCfg.value("baseUrl").toString(), agentCfg.value("apiKey").toString(),
                  agentCfg.value("model").toString()};
    QString buffer;
    m_llm->chatStream(cfg, messages, {}, {
        .onDelta = [&buffer](const QString& d) { buffer += d; },
        .onToolCalls = nullptr,
        .onError = [this, area](const QString& e) { emit failed(area, e); },
        .onDone = [this, area, &buffer]() {
            m_sessions->saveDraft(area, buffer);
            emit draftReady(area, buffer);
        }});
}
```

> 上述捕获 `&buffer` 存在悬垂风险：实现时必须把 `buffer` 放到堆上（`std::make_shared<QString>()`）并在回调中捕获 shared_ptr。

- [ ] **Step 2: 前端 DraftCard.vue + ChatPanel 接入**

```vue
<script setup lang="ts">
import { ref } from "vue";
const props = defineProps<{ prompt: string }>();
const emit = defineEmits<{ send: [text: string]; cancel: [] }>();
const editing = ref(false);
const text = ref(props.prompt);
</script>

<template>
  <div class="draft-card">
    <div class="draft-hint">优化后的提示词 · 双击编辑</div>
    <textarea v-if="editing" v-model="text" autofocus @blur="editing = false" />
    <pre v-else @dblclick="editing = true">{{ text }}</pre>
    <div class="draft-actions">
      <button @click="emit('send', text)">发送</button>
      <button @click="emit('cancel')">取消</button>
    </div>
  </div>
</template>
```

`ChatPanel.vue` 新增订阅 `draft.created` → 追加草稿卡；草稿卡 `send` → `client.request("draft.send", {area, prompt})`；`cancel` → 仅前端移除卡片。

- [ ] **Step 3: 单测以集成覆盖（Task 3 的 mock LLM 测试同时覆盖草稿流）；提交**（`feat: 总结 agent 草稿生成与持久化`）

---

### Task 3: AgentOrchestrator（Planner + Executor 两阶段）

**Files:** Create `src/core/agent/AgentOrchestrator.h/.cpp`、`web/src/components/PlanCard.vue`、`tests/tst_orchestrator.cpp`；Modify `CoreApp`（draft.send → 启动规划；plan.confirm / plan.cancel 在 Task 4 接线）。

**Interfaces:**
- 计划结构：

```cpp
struct BuildGroup { QString title; QStringList items; QString acceptance; };
struct BuildPlan { QVector<BuildGroup> groups; int maxSteps = 50; };
```

- `class AgentOrchestrator : QObject`
  - `void plan(const QString& taskId, TaskManager* tm, const QString& prompt, const QString& area);`：一次**纯文本**调用（不传 tools），解析 JSON；失败重试 1 次，仍失败降级默认四分组。
  - `void execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools, const BuildPlan& plan, const QString& prompt, const QString& targetFolder, const QString& area);`：按组「建造 → 自检」状态机执行。
  - 信号：`planReady(area, taskId, QJsonObject plan)`、`groupProgress(area, int index, int count, QString title)`、`finalText(area, QString)`、`failed(area, QString)`、`toolStep(area, tool, status)`、`delta(area, text)`。
- 事件：`plan.created {area, taskId, groups, totalSteps}`；`task.progress {area, groupIndex, groupCount, title}`；`tool.step` 沿用；结束时 `asset.created`（Task 4 编排）。
- 状态机：`GroupPhase { Build, SelfCheck }`。Build 阶段 LLM 无工具调用 → 注入本组验收标准进入 SelfCheck；SelfCheck 无工具调用 → 本组完成 → 下一组 Build 或 `finishOk`。
- CoreApp 暂存 `m_pendingDrafts[area] = {taskId, prompt}`；`plan.confirm` → 缓存计划并发 `folderPick.open`（Task 4）；`plan.cancel` → 取消任务 + `error.card {reason:"user-cancelled"}`。

- [ ] **Step 1: 规划阶段测试（红）**

```cpp
void plannerParsesGroups()
{
    MockLlmServer llm({textChunk(R"({"groups":[
        {"title":"底座与环境","items":["底座","井盖"],"acceptance":"俯视截图：底座完整"},
        {"title":"建筑外观","items":["墙体","招牌"],"acceptance":"街角截图：主体齐全"}]})")});
    AgentOrchestrator orch(&llm, nullptr, nullptr);
    QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

    orch.plan("t1", nullptr, "做一个雨夜便利店", "blender");

    QTRY_COMPARE(ready.count(), 1);
    const QJsonObject plan = ready.first().at(2).toJsonObject();
    QCOMPARE(plan.value("groups").toArray().size(), 2);
    QCOMPARE(plan.value("totalSteps").toInt(), 26);   // 8*2+10
}

void plannerFallsBackToDefaults()
{
    MockLlmServer llm({textChunk("抱歉，我无法输出 JSON。"), textChunk("仍然无法输出。")});
    AgentOrchestrator orch(&llm, nullptr, nullptr);
    QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

    orch.plan("t1", nullptr, "做一个雨夜便利店", "blender");

    QTRY_COMPARE(ready.count(), 1);
    const QJsonObject plan = ready.first().at(2).toJsonObject();
    QCOMPARE(plan.value("groups").toArray().size(), 4);   // 默认四分组
    QCOMPARE(plan.value("totalSteps").toInt(), 42);
    QCOMPARE(llm.requestCount(), 2);                     // 重试 1 次
}
```

- [ ] **Step 2: 规划实现（绿）**

```cpp
static const char* kPlanSystemPrompt =
    "你是低模场景建造的规划器。只输出一个 JSON 对象，不要任何解释或代码块围栏，格式："
    "{\"groups\":[{\"title\":\"分组名\",\"items\":[\"物体1\",\"物体2\"],\"acceptance\":\"自检时截图检查什么\"}]}。"
    "要求：3~6 组，覆盖用户提示词中全部必须独立的物体与数量下限；按施工顺序排列"
    "（底座环境 → 建筑外观 → 室内陈设 → 灯光特效）。";

static int planBudget(int groupCount)
{
    return qMin(8 * groupCount + 10, 60);
}

static BuildPlan parsePlan(const QString& text)
{
    BuildPlan plan;
    const int start = text.indexOf('{');
    const int end = text.lastIndexOf('}');
    if (start < 0 || end <= start)
        return plan;                       // 空计划 = 解析失败
    const QJsonObject obj = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8()).object();
    for (const QJsonValue& value : obj.value("groups").toArray()) {
        const QJsonObject item = value.toObject();
        BuildGroup group;
        group.title = item.value("title").toString();
        for (const QJsonValue& name : item.value("items").toArray())
            group.items << name.toString();
        group.acceptance = item.value("acceptance").toString();
        if (!group.title.isEmpty())
            plan.groups.push_back(group);
    }
    plan.maxSteps = planBudget(plan.groups.size());
    return plan;
}

static BuildPlan defaultPlan()
{
    BuildPlan plan;
    plan.groups = {
        {"底座环境", {}, "俯视截图：底座完整、路面/标线齐全、无悬空凹陷"},
        {"建筑外观", {}, "街角视角截图：主体、招牌、门窗齐全，比例协调"},
        {"店内陈设", {}, "透过橱窗截图：按提示词店内清单逐项核对"},
        {"灯光特效", {}, "夜景截图：招牌自发光、路灯、雨水粒子均已生效"},
    };
    plan.maxSteps = planBudget(plan.groups.size());
    return plan;
}

void AgentOrchestrator::plan(const QString& taskId, TaskManager* tm, const QString& prompt,
                             const QString& area)
{
    m_taskId = taskId;
    m_tm = tm;
    m_prompt = prompt;
    m_area = area;
    attemptPlan(0);
}

void AgentOrchestrator::attemptPlan(int attempt)
{
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    const QJsonArray messages{
        QJsonObject{{"role", "system"}, {"content", QString::fromUtf8(kPlanSystemPrompt)}},
        QJsonObject{{"role", "user"}, {"content", m_prompt}}};
    m_stream.clear();
    m_llm->chatStream(m_cfg, messages, {}, {
        .onDelta = [this](const QString& d) { m_stream += d; },
        .onToolCalls = nullptr,
        .onError = [this](const QString& e) { fail(e); },
        .onDone = [this, attempt]() {
            BuildPlan plan = parsePlan(m_stream);
            if (plan.groups.isEmpty() && attempt < 1) {
                attemptPlan(attempt + 1);      // 解析失败重试 1 次
                return;
            }
            if (plan.groups.isEmpty())
                plan = defaultPlan();          // 仍失败 → 降级默认分组
            QJsonArray groups;
            for (const BuildGroup& g : plan.groups)
                groups.append(QJsonObject{{"title", g.title},
                                          {"items", QJsonArray::fromStringList(g.items)},
                                          {"acceptance", g.acceptance}});
            emit planReady(m_area, m_taskId,
                           QJsonObject{{"taskId", m_taskId}, {"groups", groups},
                                       {"totalSteps", plan.maxSteps}});
        }});
}
```

- [ ] **Step 3: 执行状态机测试（红）**

```cpp
void executorBuildThenSelfChecks()
{
    MockLlmServer llm({toolCallChunk("execute_blender_code"),
                       textChunk("本组建造完成"),
                       textChunk("自检通过")});
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    BuildPlan plan;
    plan.groups = {{"底座与环境", {"底座"}, "俯视截图检查底座完整"}};
    plan.maxSteps = 18;
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, {{/* execute_blender_code schema */}}, plan,
                 "做一个雨夜便利店", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QCOMPARE(llm.requestCount(), 3);                    // 建造 → 自检 → 收尾
    QVERIFY(llm.requestAt(1).contains("验收标准"));      // 自检指令已注入
}

void executorAdvancesGroups()
{
    MockLlmServer llm({toolCallChunk("execute_blender_code"), textChunk("组1建造完成"),
                       textChunk("组1自检通过"),
                       toolCallChunk("execute_blender_code"), textChunk("组2建造完成"),
                       textChunk("组2自检通过")});
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    BuildPlan plan;
    plan.groups = {{"底座与环境", {}, "检查底座"}, {"建筑外观", {}, "检查建筑"}};
    QSignalSpy progress(&orch, &AgentOrchestrator::groupProgress);
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, {{/* schema */}}, plan, "p", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QCOMPARE(progress.count(), 2);                      // 两组各一次进度
    QCOMPARE(progress.at(1).at(1).toInt(), 2);          // 第二组 index
}
```

`MockLlmServer`（`tests/mock/mock_llm_server.h`）：构造收 SSE 响应脚本（按请求顺序逐轮返回）；`requestCount()` 与 `requestAt(i)` 返回该轮原始请求体（用于断言注入消息）。`toolCallChunk/textChunk` 为脚本包装小工具。

- [ ] **Step 4: 执行状态机实现（绿）**

```cpp
static QString blenderSystemPrompt()
{
    return QStringLiteral("你是 SynthForge Studio 的 Blender 建模 agent。使用提供的 MCP 工具"
                          "（execute_blender_code 等）完成低模场景：先清场再分组建造，"
                          "必要时截图查看结果并修正；全部完成后按规范导出并落盘。");
}

void AgentOrchestrator::execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools,
                                const BuildPlan& plan, const QString& prompt,
                                const QString& targetFolder, const QString& area)
{
    m_taskId = taskId;
    m_tm = tm;
    m_tools = tools;
    m_plan = plan;
    m_area = area;
    m_maxSteps = plan.maxSteps > 0 ? plan.maxSteps : 50;
    m_steps = 0;
    m_groupIndex = 0;
    m_phase = GroupPhase::Build;
    m_messages = {
        QJsonObject{{"role", "system"}, {"content", blenderSystemPrompt()}},
        QJsonObject{{"role", "user"},
                    {"content", QStringLiteral("已确认的专业提示词：\n%1\n目标存放目录：%2。")
                                    .arg(prompt, targetFolder)}}};
    startGroup();
}

void AgentOrchestrator::startGroup()
{
    const BuildGroup& group = m_plan.groups[m_groupIndex];
    emit groupProgress(m_area, m_groupIndex + 1, m_plan.groups.size(), group.title);
    m_messages.append(QJsonObject{
        {"role", "user"},
        {"content", QStringLiteral("开始第 %1/%2 组「%3」。本组对象：%4。完成后停止调用工具。")
                        .arg(m_groupIndex + 1)
                        .arg(m_plan.groups.size())
                        .arg(group.title,
                             group.items.isEmpty() ? QStringLiteral("按提示词清单执行")
                                                   : group.items.join(QStringLiteral("、")))}});
    m_stream.clear();
    tick();
}

void AgentOrchestrator::onTurnFinished()   // 一轮 LLM 无工具调用时进入
{
    if (m_phase == GroupPhase::Build) {
        m_phase = GroupPhase::SelfCheck;
        const BuildGroup& group = m_plan.groups[m_groupIndex];
        m_messages.append(QJsonObject{
            {"role", "user"},
            {"content", QStringLiteral("第 %1 组建造结束。请对照验收标准：%2。调用截图工具检查本组结果，"
                                       "缺漏或明显偏差立即补建修正；确认无误后停止调用工具。")
                            .arg(m_groupIndex + 1)
                            .arg(group.acceptance)}});
        m_stream.clear();
        tick();
        return;
    }
    if (m_groupIndex + 1 < m_plan.groups.size()) {
        ++m_groupIndex;
        m_phase = GroupPhase::Build;
        startGroup();
        return;
    }
    finishOk(m_stream);
}

void AgentOrchestrator::tick()
{
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    if (++m_steps > m_maxSteps) {
        fail(QStringLiteral("max steps exceeded"));
        return;
    }
    m_llm->chatStream(m_cfg, m_messages, m_tools, {
        .onDelta = [this](const QString& d) { m_stream += d; emit delta(m_area, d); },
        .onToolCalls = [this](const QJsonArray& calls) { executeToolCalls(calls); },
        .onError = [this](const QString& e) { fail(e); },
        .onDone = [this]() {
            if (m_pendingToolCalls == 0 && !m_awaitingTools)
                onTurnFinished();
        }});
}
```

`executeToolCalls` 保留原实现：

```cpp
void AgentOrchestrator::executeToolCalls(const QJsonArray& calls)
{
    m_awaitingTools = true;
    m_pendingToolCalls = calls.size();
    m_messages.append(QJsonObject{{"role", "assistant"},
                                  {"content", m_stream},
                                  {"tool_calls", calls}});
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QString name = call.value("function").toObject().value("name").toString();
        const QJsonObject args = QJsonDocument::fromJson(
            call.value("function").toObject().value("arguments").toString().toUtf8()).object();
        emit toolStep(m_area, name, "running");
        m_mcp->callTool(name, args, [this, call, name](const QJsonValue& result,
                                                       const QJsonObject& error) {
            emit toolStep(m_area, name, error.isEmpty() ? "done" : "error");
            m_messages.append(toolResultMessage(call.value("id").toString(), result, error));
            if (--m_pendingToolCalls == 0) {
                m_awaitingTools = false;
                m_stream.clear();
                tick();               // 回喂后继续下一轮
            }
        });
    }
}
```

`toolResultMessage` 核心（截图为 image content 时压缩为 JPEG base64）：

```cpp
QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error)
{
    QJsonArray content;
    if (!error.isEmpty()) {
        content.append(QJsonObject{{"type", "text"},
                                   {"text", "ERROR: " + QString::fromUtf8(
                                                QJsonDocument(error).toJson(QJsonDocument::Compact))}});
    } else {
        for (const QJsonValue& item : result.toObject().value("content").toArray()) {
            const QJsonObject obj = item.toObject();
            if (obj.value("type").toString() == "image") {
                const QByteArray jpeg = compressImage(
                    QByteArray::fromBase64(obj.value("data").toString().toUtf8()));
                content.append(QJsonObject{{"type", "image_url"},
                                           {"image_url", QJsonObject{{"url",
                                               "data:image/jpeg;base64," + QString::fromLatin1(jpeg.toBase64())}}}});
            } else {
                content.append(obj);
            }
        }
    }
    return QJsonObject{{"role", "tool"}, {"tool_call_id", callId}, {"content", content}};
}

QByteArray compressImage(const QByteArray& png)
{
    QImage image;
    if (!image.loadFromData(png))
        return {};
    if (image.width() > 1024 || image.height() > 1024)
        image = image.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return out;
}
```

- [ ] **Step 5: 前端 PlanCard.vue + ChatPanel 接入**

```vue
<script setup lang="ts">
import { ref } from "vue";
import type { WsClient } from "../ws";

const props = defineProps<{
  client: WsClient;
  area: string;
  plan: { taskId: string; groups: { title: string; items: string[]; acceptance: string }[]; totalSteps: number };
}>();
const decided = ref<"pending" | "started" | "cancelled">("pending");

function start() {
  decided.value = "started";
  props.client.request("plan.confirm", { area: props.area, taskId: props.plan.taskId });
}
function cancel() {
  decided.value = "cancelled";
  props.client.request("plan.cancel", { area: props.area, taskId: props.plan.taskId });
}
</script>

<template>
  <div class="plan-card">
    <div class="plan-hint">建造计划 · 预估 {{ plan.totalSteps }} 步</div>
    <ol>
      <li v-for="(g, i) in plan.groups" :key="i">
        <div class="group-title">{{ i + 1 }}. {{ g.title }}</div>
        <div v-if="g.items.length" class="group-items">{{ g.items.join(" / ") }}</div>
        <div class="group-accept">验收：{{ g.acceptance }}</div>
      </li>
    </ol>
    <div v-if="decided === 'pending'" class="plan-actions">
      <button @click="start">开始生成</button>
      <button @click="cancel">取消</button>
    </div>
    <div v-else class="plan-state">
      {{ decided === "started" ? "已确认，等待选择存放位置…" : "已取消" }}
    </div>
  </div>
</template>
```

`ChatPanel.vue` 新增订阅 `plan.created` → 追加计划卡；计划卡 `start/cancel` → `client.request("plan.confirm"|"plan.cancel", {area, taskId})`；收到 `folderPick.open`（Task 4）后弹存放位置。

- [ ] **Step 6: CMake + 跑测试 + Commit**（`tst_orchestrator` 4 用例：规划解析 / 降级 / 单组执行 / 多组推进；`feat: Planner→Executor 两阶段主循环（计划卡/分组自检）`）

---

### Task 4: folderPick 流程 + 资产落盘

**Files:** Create `src/core/assets/AssetLibrary.h/.cpp`、`web/src/components/FolderPicker.vue`；Modify `CoreApp`（`folderPick.submit/cancel`、`tree.refresh`、`asset.import` 预留）。

**Interfaces:**
- 事件 `folderPick.open {requestId, rootName:"ModelLibrary", tree}`（tree 由 AssetLibrary::folderTree() 生成，`{name, path, children}`）。
- 请求 `folderPick.submit {requestId, relativePath}` → 校验（必须已存在、必须在根内）→ 调用 `orchestrator.execute(taskId, tm, tools, plan, prompt, targetFolder, area)`（plan/prompt 来自 `plan.confirm` 时缓存）。
- 请求 `folderPick.cancel {requestId}` → 任务结束，广播 `error.card {reason:"user-cancelled"}`。
- orchestrator 完成后：`bpy.ops.export_scene.gltf(filepath=<root>/<rel>/<name>.glb, export_format='GLB')` 经 Blender MCP `execute_blender_code` 执行；广播 `asset.created {name, paths:{glb, blend?}}` 并触发 `fileTree.changed {area:"blender"}`。

- [ ] **Step 1: 实现**

```cpp
class AssetLibrary
{
public:
    explicit AssetLibrary(QString root);          // Documents/SynthForgeStudio/ModelLibrary
    QJsonObject folderTree() const;               // 递归目录树（仅目录）
    bool isValidTarget(const QString& relative) const; // canonical 前缀校验，防 ../ 逃逸
    QString absoluteFor(const QString& relative) const;
    QStringList scanGlb() const;                  // 供文件树刷新
};
```

`folderTree` 递归 `QDirIterator`（`QDir::Dirs | NoDotAndDotDot`），限制深度 6；`isValidTarget` 用 `QFileInfo(candidate).canonicalFilePath()` 前缀比对根目录，拒绝 `..`。

`CoreApp` 里 `plan.confirm` 处理：分配 `requestId = ++m_seq`，暂存 `{area, taskId, prompt, plan, requestId}` 于 `m_pendingFolderPicks`，广播 `folderPick.open`；`folderPick.submit` 校验通过后取回暂存，调用 `orchestrator.execute(...)`。`draft.send` 在 Task 3 中只负责启动规划与暂存草稿。

- [ ] **Step 2: 前端 FolderPicker.vue**

弹窗树（缩进列表 + 单选），底部 `[取消] [确认]`；确认发 `folderPick.submit`。样式复用设置弹窗的遮罩类。

- [ ] **Step 3: 手工验证**（真实 Blender：发送需求 → 草稿 → 发送 → 计划卡确认 → 选子文件夹 → 模型按组生成并落盘 → `asset.created` 触发文件树刷新）

- [ ] **Step 4: Commit**（`feat: 存放位置确认框与 GLB 资产落盘`）

---

### Task 5: 文件树数据源 + three.js GLB 预览

**Files:** Modify `web/src/components/FileTree.vue`、`PreviewPane.vue`；Modify `CoreApp`（`fileTree.changed` 事件 + `session.load` 之外新增 `assets.list` 请求）。

**Interfaces:**
- 请求 `assets.list {}` → `[{name, glb, relativePath}]`；事件 `fileTree.changed {area:"blender"}` 触发重新拉取。
- `PreviewPane`：加载所选 GLB（`file:///` URL），GLTFLoader + OrbitControls，窗口 resize 自适应，加载失败显示占位文案。

- [ ] **Step 1: 实现 PreviewPane 核心**

```vue
<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref, watch } from "vue";
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

const props = defineProps<{ glbUrl?: string }>();
const host = ref<HTMLDivElement>();

let renderer: THREE.WebGLRenderer;
let scene: THREE.Scene;
let camera: THREE.PerspectiveCamera;
let controls: OrbitControls;
let frame = 0;

function resize() {
  if (!host.value) return;
  renderer.setSize(host.value.clientWidth, host.value.clientHeight);
  camera.aspect = host.value.clientWidth / host.value.clientHeight;
  camera.updateProjectionMatrix();
}

onMounted(() => {
  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000);
  camera.position.set(2, 2, 3);
  controls = new OrbitControls(camera, renderer.domElement);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x444444, 1));
  host.value!.appendChild(renderer.domElement);
  window.addEventListener("resize", resize);
  resize();
  const loop = () => { frame = requestAnimationFrame(loop); controls.update(); renderer.render(scene, camera); };
  loop();
});

watch(() => props.glbUrl, (url) => {
  if (!url) return;
  scene.children = scene.children.filter((c) => !(c instanceof THREE.Group));
  new GLTFLoader().load(url, (gltf) => scene.add(gltf.scene));
});

onBeforeUnmount(() => {
  cancelAnimationFrame(frame);
  renderer.dispose();
});
</script>

<template><div ref="host" class="preview" /></template>
```

依赖增量：`web/package.json` 加 `"three": "^0.169.0"`、`"@types/three": "^0.169.0"`。

`FileTree.vue`：树形列表（文件夹可折叠 + `.glb` 文件项），点击文件 emit `select(url)` → `App.vue` 传给 `PreviewPane`。

- [ ] **Step 2: 手工验收（P4 总验收）**

1. 真实链路：Blender 打开且插件运行；发送「做三个低模木箱」→ 草稿卡出现；双击可编辑；发送 → 计划卡出现（分组/验收标准可读）→ [开始生成] → 选 `Props/` 子目录 → 步骤卡逐步更新 + `task.progress` 显示当前组 → 对话区出现截图 → 完成后文件树出现 `Props/xxx.glb`，点击可预览旋转。
2. 拒绝路径：关掉 Blender 插件 → 发送 → 立刻出现 `blender-not-ready` 错误卡，无总结 agent 请求。

- [ ] **Step 3: Commit**（`feat: 资产文件树与 three.js GLB 预览`）

---

### Task 6: 修改已有资产（跳过草稿/计划）

**Files:** Modify `CoreApp`（`chat.send` 增加 `mode:"modify"` + `asset` 参数）、`web/src/components/ChatPanel.vue`（目标资产 chip）、`App.vue`（FileTree 选中项传给 ChatPanel）；无新 C++ 模块（复用 `AgentOrchestrator::execute`）。

**Interfaces:**
- 前端：FileTree 选中 `.glb` → ChatPanel 输入框上方显示 `目标资产：<name> ✕` chip；发送 `chat.send {area:"blender", mode:"modify", asset:"道具/sword_01/sword_01.glb", text:"把剑改短一点"}`。
- CoreApp：`mode=="modify"` → `checkLowpoly()` → 校验 `asset` 在库内且存在（否则返回 `{"ok":false, "error":"asset-not-found"}`，不占 error.card 字典）→ 构造单组计划并调 `execute()`，**不经过** SummarizerAgent、plan 卡、folderPick。
- prompt 包装：`"打开并修改已有资产 <绝对路径>。要求：<用户原话>。完成后覆盖导出同名 GLB/FBX 并保存 .blend。"`；`targetFolder = 资产所在目录`。
- 完成后 `asset.created` 同名覆盖并触发 `fileTree.changed`。

- [ ] **Step 1: CoreApp 接线**

```cpp
if (mode == "modify") {
    const QString asset = params.value("asset").toString();
    const QFileInfo info(m_library->absoluteFor(asset));
    if (!info.exists() || !m_library->isValidTarget(QFileInfo(asset).path()))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "asset-not-found"}});
    BuildPlan plan;
    plan.groups = {{"修改资产", {}, "修改意图达成；同名 GLB/FBX/blend 已覆盖"}};
    plan.maxSteps = 50;
    const QString prompt = QStringLiteral("打开并修改已有资产 %1。要求：%2。"
                                          "完成后覆盖导出同名 GLB/FBX 并保存 .blend。")
                               .arg(info.absoluteFilePath(), params.value("text").toString());
    m_orch->execute(taskId, tm, blenderTools, plan, prompt, info.absolutePath(), area);
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}
```

- [ ] **Step 2: ChatPanel 目标资产 chip**

```ts
const props = defineProps<{ area: string; client: WsClient; selectedAsset?: string }>();

function send() {
  const mode = props.selectedAsset ? "modify" : "lowpoly";
  props.client.request("chat.send", {
    area: props.area, mode, text: input.value, asset: props.selectedAsset,
  });
}
```

```vue
<div v-if="selectedAsset" class="asset-chip">
  目标资产：{{ selectedAsset.split("/").pop() }}
  <button @click="emit('clearAsset')">✕</button>
</div>
```

`App.vue` 把 FileTree 的 `select` 结果同时传给 `ChatPanel:selected-asset`（与 PreviewPane 共用选中状态）；chip 的 `✕` 清空选中。

- [ ] **Step 3: 手工验证**（选中已有资产 → 输入修改指令 → 步骤卡执行 → 检查 .glb 修改时间与预览更新 → 文件树刷新；确认全程未出现草稿卡/计划卡/位置弹窗）

- [ ] **Step 4: Commit**（`feat: 修改已有资产（跳过草稿与计划，直接执行）`）

---

## P4 完成标准

- [ ] 低模全链真实跑通（发送 → 草稿 → 计划卡确认 → 确认目录 → 分组建模/自检 → 截图 → 落盘 → 预览）
- [ ] 修改已有资产路径跑通，且全程无草稿卡/计划卡/位置弹窗
- [ ] 规划解析失败自动降级默认四分组，流程不中断；`plan.cancel` 正确取消任务
- [ ] 前置检查先于总结 agent；`blender-not-ready` 时零 LLM 调用
- [ ] 截图压缩后回喂（长边 ≤1024、JPEG q80）
- [ ] 取消任务后 MCP 调用在工具边界停止，任务事件 `finished(cancelled=true)`
- [ ] `tst_preflight`、`tst_orchestrator` 全绿
