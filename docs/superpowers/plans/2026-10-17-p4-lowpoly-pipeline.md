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
src/core/mcp/McpRuntime.h/.cpp               ← 桥接运行时（工具缓存/重连状态/主动重启，Task 0）
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
tests/tst_mcp_runtime.cpp                    ← mock server 握手/工具缓存/重启
tests/tst_asset_library.cpp                  ← 资产库文件操作与越界校验
tests/tst_orchestrator.cpp                   ← mock MCP + mock LLM 集成
```

---

### Task 0: McpRuntime（桥接运行时：握手 / 工具缓存 / 重连状态 / 主动重启）

> **为何存在**：P1 只提供传输与客户端原语，P2 未组装 MCP 运行时。本任务把「传输 + 客户端 + 工具清单缓存 + 重连状态」封成一个可复用类；P4 的 `blender` 与 P6 的 `unity` 各持一个实例，`blenderTools` 的唯一来源就是它。

**Files:** Create `src/core/mcp/McpRuntime.h/.cpp`、`tests/tst_mcp_runtime.cpp`；Modify `CMakeLists.txt`。

**Interfaces:**
- Consumes: `McpClient`（P1 Task 3/6）、`McpTransport` / `StdioTransport`（P1 Task 4）、`mock_mcp_server`（P1 Task 4）。
- Produces:
  - `class McpRuntime : QObject`；构造 `McpRuntime(area, std::unique_ptr<McpTransport>, QObject* parent = nullptr)`，`area ∈ {"blender","unity"}`。
  - `bool start()`（启动传输并握手）、`bool restart()`（`client->shutdown()` + `transport->start()`，供 `bridge.restart` 使用）、`void stop()`（`client->shutdown()`，供切换工程断链；需要时再 `start()`）。
  - `McpClient* client() const`（供 `AgentOrchestrator` 构造与工具调用复用）。
  - `bool isReady() const`、`QJsonArray tools() const`（最近一次 `tools/list` 缓存）、`void refreshTools()`。
  - 信号 `toolsChanged(QJsonArray)`、`statusChanged(area, status, hint)`（status ∈ `off|reconnecting|ready`；P7 直接转发为 `conn.status`）。
  - 状态映射：`client->ready/reconnected` → 先 `refreshTools()` 再 `statusChanged(area,"ready","")`；`reconnecting(attempt)` → `statusChanged(area,"reconnecting","第 N 次重连")`；`failed(reason)` → `statusChanged(area,"off",reason)`。

- [ ] **Step 1: 写失败测试**

`tests/tst_mcp_runtime.cpp`：

```cpp
#include <QProcessEnvironment>
#include <QtTest>

#include "core/mcp/McpRuntime.h"
#include "core/mcp/StdioTransport.h"

class McpRuntimeTest : public QObject
{
    Q_OBJECT
private slots:
    void handshakeCachesToolsAndRestarts()
    {
        BridgeCommand command{QString::fromLocal8Bit(MOCK_SERVER_PATH), {},
                              QProcessEnvironment::systemEnvironment()};
        McpRuntime runtime("blender", std::make_unique<StdioTransport>(command));
        QSignalSpy statusSpy(&runtime, &McpRuntime::statusChanged);

        QVERIFY(runtime.start());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isReady(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.tools().size() == 1, 15000);

        QVERIFY(runtime.restart());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isReady(), 15000);

        int readyCount = 0;
        for (const auto& args : statusSpy)
            if (args.at(1).toString() == QStringLiteral("ready"))
                ++readyCount;
        QVERIFY2(readyCount >= 2, "首次握手与重启后都应广播 ready");
    }
};

QTEST_MAIN(McpRuntimeTest)
#include "tst_mcp_runtime.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --config Debug --target tst_mcp_runtime`
Expected: 编译失败（`McpRuntime.h` 不存在）。

- [ ] **Step 3: 实现**

`src/core/mcp/McpRuntime.h`：

```cpp
#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

#include <memory>

class McpClient;
class McpTransport;

class McpRuntime : public QObject
{
    Q_OBJECT

public:
    McpRuntime(const QString& area, std::unique_ptr<McpTransport> transport,
               QObject* parent = nullptr);
    ~McpRuntime() override;

    bool start();
    bool restart(); // 主动重启：不触发自动重连的退避，由调用方直接拉起
    void stop();    // 主动断开：关心跳，供切换工程；再次 start() 才恢复
    McpClient* client() const { return m_client; }
    bool isReady() const;
    QJsonArray tools() const { return m_tools; }
    void refreshTools();

signals:
    void toolsChanged(const QJsonArray& tools);
    void statusChanged(const QString& area, const QString& status, const QString& hint);

private:
    QString m_area;
    std::unique_ptr<McpTransport> m_transport;
    McpClient* m_client;
    QJsonArray m_tools;
};
```

`src/core/mcp/McpRuntime.cpp`：

```cpp
#include "core/mcp/McpRuntime.h"

#include "core/mcp/McpClient.h"
#include "core/mcp/McpTransport.h"

McpRuntime::McpRuntime(const QString& area, std::unique_ptr<McpTransport> transport,
                       QObject* parent)
    : QObject(parent), m_area(area), m_transport(std::move(transport)),
      m_client(new McpClient(m_transport.get(), this))
{
    connect(m_transport.get(), &McpTransport::started, m_client, &McpClient::start);
    connect(m_client, &McpClient::ready, this, [this] {
        refreshTools();
        emit statusChanged(m_area, QStringLiteral("ready"), QString());
    });
    connect(m_client, &McpClient::reconnected, this, [this] {
        refreshTools();
        emit statusChanged(m_area, QStringLiteral("ready"), QString());
    });
    connect(m_client, &McpClient::reconnecting, this, [this](int attempt) {
        emit statusChanged(m_area, QStringLiteral("reconnecting"),
                           QStringLiteral("第 %1 次重连").arg(attempt));
    });
    connect(m_client, &McpClient::failed, this, [this](const QString& reason) {
        emit statusChanged(m_area, QStringLiteral("off"), reason);
    });
}

McpRuntime::~McpRuntime() = default;

bool McpRuntime::start()
{
    m_transport->start();
    return true;
}

void McpRuntime::stop()
{
    m_client->shutdown(); // 主动断开：不触发自动重连
    // 顶栏灯变灰；调用方在主动 stop 前先 m_everReady.remove(area)，避免误报 bridge-crashed
    emit statusChanged(m_area, QStringLiteral("off"), QString());
}

bool McpRuntime::restart()
{
    m_client->shutdown();       // 主动断开：抑制旧会话的自动重连，不报 bridge-crashed
    m_client->prepareRestart(); // bridge.restart 语义：启动失败仍可退避重试（不被 shutdown 标志卡死）
    m_transport->start();       // started → McpClient::start 重新握手
    return true;
}

bool McpRuntime::isReady() const
{
    return m_client->isReady();
}

void McpRuntime::refreshTools()
{
    m_client->listTools([this](const QJsonArray& tools) {
        m_tools = tools;
        emit toolsChanged(m_tools);
    });
}
```

- [ ] **Step 4: CMake + 跑测试**

core 源文件追加 `src/core/mcp/McpRuntime.cpp`；追加 target：

```cmake
add_executable(tst_mcp_runtime tests/tst_mcp_runtime.cpp)
target_link_libraries(tst_mcp_runtime PRIVATE synthforge_core Qt6::Test)
target_compile_definitions(tst_mcp_runtime PRIVATE
    MOCK_SERVER_PATH="$<TARGET_FILE:mock_mcp_server>")
add_dependencies(tst_mcp_runtime mock_mcp_server)
add_test(NAME tst_mcp_runtime COMMAND tst_mcp_runtime)
```

Run: `ctest --test-dir build -C Debug -R tst_mcp_runtime --output-on-failure`
Expected: 1 用例 Passed。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/core/mcp/McpRuntime.h src/core/mcp/McpRuntime.cpp tests/tst_mcp_runtime.cpp
git commit -m "feat: McpRuntime 桥接运行时（工具缓存/重连状态/主动重启）"
```

---

### Task 1: PreflightChecker + chat.send 模式分支

**Files:** Create `src/core/preflight/PreflightChecker.h/.cpp`、`tests/tst_preflight.cpp`；Modify `src/core/coreapp/CoreApp.h/.cpp`（创建 `m_blender` 运行时、`chat.send` 增加 `mode` 分支、注册 `conn.status` 与 `bridge.restart`）。

**Interfaces:**
- `struct PreflightResult { bool ok; QString reason; }`
- `class PreflightChecker { explicit PreflightChecker(SettingsStore*, McpRuntime* blender); PreflightResult checkLowpoly(); PreflightResult checkCloud3d(); }`（`blender == nullptr || !blender->isReady()` → `blender-not-ready`）
- `chat.send` 新参数：`{area, text, mode}`（`mode ∈ {"lowpoly","cloud3d","modify"}`；**未传 mode 时跳过前置检查走 P2 通用流**，仅用于兼容既有测试与无模式调用，P3 前端总会显式传）。检查失败返回 `{"accepted":false}` 并广播 `error.card {area, reason}`。
- CoreApp 新增成员 `McpRuntime* m_blender`：`start()` 中用 `defaultBridgeCommand("blender")` + `StdioTransport` 构造并 `start()`；`statusChanged` 转发为 `conn.status {blender, unity, hints}` 事件（P7 细化双灯）；`registerBridge("blender", [this]{ return m_blender->restart(); })`。
- `blenderTools` 的唯一来源：`m_blender->tools()`（`toolsChanged` 后缓存到成员 `m_blenderTools`）。

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
class McpRuntime;

struct PreflightResult
{
    bool ok = false;
    QString reason; // error.card 的 reason 字段
};

class PreflightChecker
{
public:
    // blenderRuntime 为 nullptr 或未就绪均视为链未就绪。
    PreflightChecker(SettingsStore* settings, McpRuntime* blenderRuntime);

    PreflightResult checkLowpoly() const;   // Blender 链就绪
    PreflightResult checkCloud3d() const;   // 当前 3D 服务商必填项完整

private:
    SettingsStore* m_settings;
    McpRuntime* m_blenderRuntime;
};
```

`checkCloud3d` 判定：`cloud3d.provider` 为 `tripo` 需 `apiKey` 非空；`hunyuan` 需 `secretId`+`secretKey`；`custom` 需 `baseUrl` 非空；缺失返回 `{false, "cloud3d-not-configured"}`。`checkLowpoly` 判定：`m_blenderRuntime && m_blenderRuntime->isReady()`，否则 `{false, "blender-not-ready"}`。

CoreApp 的 `start()` 中先创建运行时（handler 注册之前）：

```cpp
m_blender = new McpRuntime(QStringLiteral("blender"),
                           std::make_unique<StdioTransport>(defaultBridgeCommand("blender")),
                           this);
connect(m_blender, &McpRuntime::toolsChanged, this,
        [this](const QJsonArray& tools) {
            m_blenderTools = tools;
            if (m_blender->isReady())   // 顶栏悬停文案：已就绪 · N 个工具（规格 4.2）
                broadcastConnStatus(QStringLiteral("blender"), QStringLiteral("ready"),
                                    QStringLiteral("已就绪 · %1 个工具").arg(tools.size()));
        });
connect(m_blender, &McpRuntime::statusChanged, this, &CoreApp::broadcastConnStatus);
registerBridge(QStringLiteral("blender"), [this] { return m_blender->restart(); });
m_preflight = new PreflightChecker(m_settings, m_blender);
// 末尾统一启动：
// m_blender->start();
```

> 成员：`McpRuntime* m_blender = nullptr; PreflightChecker* m_preflight = nullptr; QJsonArray m_blenderTools; QHash<QString, QString> m_status, m_hints; QSet<QString> m_everReady, m_crashNotified;`（`m_everReady` = 曾握手成功的区，用于将“就绪后突然 off”映射为 `bridge-crashed` 错误卡，主动 stop/首次启动失败不报；`m_crashNotified` = 本轮断链已出卡，恢复 ready 时清除，避免重试途中反复弹卡）。`#include <QVariantMap>`。`QVariantMap{m_hints.begin(), m_hints.end()}` 在 MSVC 下可用（QHash 迭代器对为 `std::pair`）；若编译报错改用循环填充。

`CoreApp.h` 增加成员函数声明 `void broadcastConnStatus(const QString& area, const QString& status, const QString& hint);`，实现（P6 的 `m_unity` 复用）：

```cpp
void CoreApp::broadcastConnStatus(const QString& area, const QString& status,
                                  const QString& hint)
{
    m_status.insert(area, status);
    if (hint.isEmpty())
        m_hints.remove(area);        // ready 时清掉旧的“第 N 次重连”提示
    else
        m_hints.insert(area, hint);
    m_ws->broadcastEvent("conn.status",
                         {{"blender", m_status.value("blender", "off")},
                          {"unity", m_status.value("unity", "off")},
                          {"hints", QJsonObject::fromVariantMap(
                                        QVariantMap{m_hints.begin(), m_hints.end()})}});
    if (status == QLatin1String("ready")) {
        m_everReady.insert(area);
        m_crashNotified.remove(area);        // 恢复后再次崩溃会重新出卡
    } else if (status == QLatin1String("off") && m_everReady.contains(area)
               && !m_crashNotified.contains(area)) {
        m_crashNotified.insert(area);        // 一次断链只报一张卡（重试循环不重复弹）
        m_ws->broadcastEvent("error.card",
                             {{"area", area}, {"reason", "bridge-crashed"},
                              {"message", hint}});
    }
}
```

`CoreApp::primaryAgentConfig()`（P4/P5/P6 共用，避免每个调用点重复解包 settings）：

```cpp
LlmConfig CoreApp::primaryAgentConfig() const
{
    const QJsonObject p = m_settings->data().value("agent").toObject()
                              .value("primary").toObject();
    return {p.value("baseUrl").toString(), p.value("apiKey").toString(),
            p.value("model").toString()};
}
```

> `CoreApp.h` 追加 `LlmConfig primaryAgentConfig() const;`（`LlmConfig` 来自 P2 `core/llm/LlmGateway.h`）。

CoreApp 的 `chat.send` / `handleChatSend` 在落库之前插入：

```cpp
const QString mode = params.value("mode").toString();   // 空 = P2 通用流（兼容测试）
if (mode == "lowpoly" || mode == "cloud3d" || mode == "modify") {
    const PreflightResult pre = mode == "cloud3d" ? m_preflight->checkCloud3d()
                                                  : m_preflight->checkLowpoly();
    if (!pre.ok) {
        m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", pre.reason}});
        return QJsonValue(QJsonObject{{"accepted", false}});
    }
}
// lowpoly：进入 Task 2 的总结 agent；cloud3d：P5 前暂回 {"accepted":false} 占位错误
```

> P2 的 `tst_headless_ws` 发 `chat.send` 时不传 `mode`，因此仍走通用流、不触发前置检查；若测试显式传 `lowpoly` 需先注入可就绪的 mock Blender 链。

- [ ] **Step 3: CMake + 测试**（`tst_preflight`，2 用例；`SettingsStore` 测试辅助：直接构造临时目录实例并 load）

- [ ] **Step 4: Commit**（`feat: 发送前置检查（Blender 链 / 云配置）`）

---

### Task 2: SummarizerAgent（草稿流）

**Files:** Create `src/core/agent/SummarizerAgent.h/.cpp`、`src/resources/prompts/summarizer.md`（内容以规格附录参考提示词为骨架，把「雨夜便利店」替换为 `{{user_request}}` 与 `{{mode}}` 模板变量）；Modify `CoreApp`（`chat.send` lowpoly 分支 → 调用；新增 `draft.send`、`draft.cancel` handler；**不引入 `draft.save`**）。

**Interfaces:**
- `class SummarizerAgent : QObject { SummarizerAgent(LlmGateway*, SessionStore*, SettingsStore*, QObject* parent = nullptr); void generate(area, userText, mode = "lowpoly"); signals: void draftReady(area, QString prompt); void failed(area, QString reason); }`
- `generate` 按 `mode` 选模板：`lowpoly → :/prompts/summarizer.md`，`cloud3d → :/prompts/summarizer-cloud3d.md`（P5 创建）。
- WS 交互：完成后广播 `draft.created {area, taskId, prompt}`；前端编辑后直接发 `draft.send {area, prompt, mode}`（CoreApp 先 `saveDraft(area, prompt)` 再进入 Task 3 规划阶段；`mode ∈ lowpoly|cloud3d`）；`draft.cancel {area, taskId}` → 丢弃本区待确认草稿（前端移除卡片），不发 `error.card`。规划完成并 `plan.confirm` 后才走 folderPick（Task 4），再由 `execute()` 执行。不引入 `draft.save`（避免规格方法族外的额外方法）。
- `chat.send` 分支顺序（`handleChatSend` 内）：前置检查 → `modify`（Task 6）→ `lowpoly` → `cloud3d`（P5）→ P2 通用流式（作为无模式时的兼容路径）；**删除 P2 版本顶部的统一 `if (appendUser)` 块**，由各分支自己落库（lowpoly/cloud3d/modify/unity 均在分支内处理；通用流 else 分支保留 P2 原块），否则低模/云模式会重复写 user 消息。

- [ ] **Step 1: 实现**

`SummarizerAgent.h`：

```cpp
class SummarizerAgent : public QObject
{
    Q_OBJECT
public:
    SummarizerAgent(LlmGateway* llm, SessionStore* sessions,
                    SettingsStore* settings, QObject* parent = nullptr);
    void generate(const QString& area, const QString& userText,
                  const QString& mode = QStringLiteral("lowpoly"));

signals:
    void draftReady(const QString& area, const QString& prompt);
    void failed(const QString& area, const QString& reason);

private:
    QJsonObject resolveConfig() const;   // agent.summarizer 未配置 → 回退 agent.primary
    LlmGateway* m_llm;
    SessionStore* m_sessions;
    SettingsStore* m_settings;
};
```

```cpp
void SummarizerAgent::generate(const QString& area, const QString& userText,
                               const QString& mode)
{
    const QJsonObject agentCfg = resolveConfig();
    const QString templatePath = mode == QLatin1String("cloud3d")
                                     ? QStringLiteral(":/prompts/summarizer-cloud3d.md")
                                     : QStringLiteral(":/prompts/summarizer.md");
    QFile tpl(templatePath);
    tpl.open(QIODevice::ReadOnly);
    QString system = QString::fromUtf8(tpl.readAll());
    system.replace("{{user_request}}", userText);
    system.replace("{{mode}}", mode);

    QJsonArray messages{QJsonObject{{"role", "system"}, {"content", system}},
                        QJsonObject{{"role", "user"}, {"content", userText}}};
    LlmConfig cfg{agentCfg.value("baseUrl").toString(), agentCfg.value("apiKey").toString(),
                  agentCfg.value("model").toString()};
    auto buffer = std::make_shared<QString>();   // 堆上缓冲，回调生命周期安全
    m_llm->chatStream(cfg, messages, {}, {
        .onDelta = [buffer](const QString& d) { buffer->append(d); },
        .onToolCalls = nullptr,
        .onError = [this, area](const QString& e) { emit failed(area, e); },
        .onDone = [this, area, buffer]() {
            m_sessions->saveDraft(area, *buffer);
            emit draftReady(area, *buffer);
        }});
}
```

CoreApp 接线（成员 `SummarizerAgent* m_summarizer = nullptr;`，在 `m_orch` 之前创建）：

```cpp
m_summarizer = new SummarizerAgent(m_llm, m_sessions, m_settings, this);
connect(m_summarizer, &SummarizerAgent::draftReady, this,
        [this](const QString& area, const QString& prompt) {
            m_ws->broadcastEvent("draft.created",
                                 {{"area", area},
                                  {"taskId", QStringLiteral("draft-%1").arg(++m_seq)},
                                  {"prompt", prompt}});
        });
connect(m_summarizer, &SummarizerAgent::failed, this,
        [this](const QString& area, const QString& reason) {
            m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", reason}});
        });
```

`handleChatSend` 的 lowpoly 分支（Task 1 前置检查与 Task 6 modify 分支之后）：

```cpp
if (mode == "lowpoly") {
    if (appendUser) {
        m_sessions->appendMessage(area, "user", text);
        m_ws->broadcastEvent("chat.message",
                             {{"area", area}, {"role", "user"}, {"content", text}});
    }
    const LlmConfig cfg = primaryAgentConfig();
    if (cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
        m_ws->broadcastEvent("error.card",
                             {{"area", area}, {"reason", "agent-not-configured"}});
        return QJsonValue(QJsonObject{{"accepted", false}});
    }
    m_summarizer->generate(area, text);   // 草稿经 draft.created 下发
    return QJsonValue(QJsonObject{{"accepted", true}});
}
```

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

`ChatPanel.vue` 新增订阅 `draft.created` → 追加草稿卡并 `busy.value = false`（后端已停止，等待用户编辑）；草稿卡 `send` → `busy.value = true` 后 `client.request("draft.send", {area, prompt, mode})`（规划中可点「停止」→ `chat.cancel`）；`cancel` → `client.request("draft.cancel", {area, taskId})` 并移除卡片。

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
  - 信号：`planReady(area, taskId, QJsonObject plan)`、`groupProgress(area, int index, int count, QString title)`、`finalText(area, QString)`、`failed(area, QString)`、`toolStep(area, tool, status)`、`delta(area, text)`、`imageReceived(area, taskId, dataUrl, caption)`、`artifactsReady(area, QStringList absoluteGlbs)`（结束时扫描目标目录新 GLB，供 CoreApp 广播 `asset.created` / `fileTree.changed`）。
  - `void setToolCallHook(std::function<void(const QString& name, bool running)> hook)` — P6 注入：工具调用前 running=true（进入抑制窗口），回调返回后 running=false（结束抑制）。
  - `void setConversationHistory(const QJsonArray& history)` — 规格 8.4 上下文组装：`system + 本区最近 20 条纯文本历史 + 本次任务指令`；CoreApp 在每次 `plan()/execute()` 前用 `m_sessions->recentMessages(area, 20)` 注入（成员 `QJsonArray m_history`）。
- 事件：`plan.created {area, taskId, groups, totalSteps}`；`task.progress {area, groupIndex, groupCount, title}`；`tool.step` 沿用；结束时 `asset.created`（Task 4 编排）。
- 状态机：`GroupPhase { Build, SelfCheck }`。Build 阶段 LLM 无工具调用 → 注入本组验收标准进入 SelfCheck；SelfCheck 无工具调用 → 本组完成 → 下一组 Build 或 `finishOk`。
- CoreApp 暂存 `m_pendingPlans[area]`（planReady 的 JSON）与 `m_planPrompts[area]`（已确认提示词）；`plan.confirm` → 转 `BuildPlan` 存入 `m_pendingPicks[area]` 并发 `folderPick.open`（Task 4 Step 1）；`plan.cancel` → 置 `m_planCancelled` + `error.card {reason:"user-cancelled"}`。

`src/core/agent/AgentOrchestrator.h`（完整声明；后续片段均在此头文件内）：

```cpp
#pragma once

#include "core/llm/LlmGateway.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class TaskManager;
class McpClient;

struct BuildGroup { QString title; QStringList items; QString acceptance; };
struct BuildPlan { QVector<BuildGroup> groups; int maxSteps = 50; };

class AgentOrchestrator : public QObject
{
    Q_OBJECT
public:
    AgentOrchestrator(LlmGateway* llm, McpClient* mcp, QObject* parent = nullptr);

    void setModelConfig(const LlmConfig& cfg) { m_cfg = cfg; }
    void plan(const QString& taskId, TaskManager* tm, const QString& prompt, const QString& area);
    void execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools,
                 const BuildPlan& plan, const QString& prompt,
                 const QString& targetFolder, const QString& area);
    void setToolCallHook(std::function<void(const QString& name, bool running)> hook);
    void setConversationHistory(const QJsonArray& history);
    bool isPlanning(const QString& area) const { return m_planning.contains(area); }

signals:
    void planReady(const QString& area, const QString& taskId, const QJsonObject& plan);
    void groupProgress(const QString& area, int index, int count, const QString& title);
    void finalText(const QString& area, const QString& text);
    void failed(const QString& area, const QString& reason);
    void toolStep(const QString& area, const QString& tool, const QString& status);
    void delta(const QString& area, const QString& text);
    void imageReceived(const QString& area, const QString& taskId,
                       const QString& dataUrl, const QString& caption);
    void artifactsReady(const QString& area, const QStringList& absoluteGlbs);

private:
    enum class GroupPhase { Build, SelfCheck };

    QJsonArray initialMessages(const QString& system, const QString& user) const;
    void attemptPlan(int attempt);
    void startGroup();
    void tick();
    void onTurnFinished();
    void executeToolCalls(const QJsonArray& calls);
    void finishOk(const QString& text);
    void finishCancelled();
    void fail(const QString& reason);
    QStringList collectNewGlbs() const;

    LlmGateway* m_llm;
    McpClient* m_mcp;
    TaskManager* m_tm = nullptr;
    LlmConfig m_cfg;
    QJsonArray m_tools;
    QJsonArray m_history;
    QJsonArray m_messages;
    BuildPlan m_plan;
    QString m_taskId;
    QString m_area;
    QString m_prompt;
    QString m_targetFolder;
    QString m_stream;
    int m_steps = 0;
    int m_maxSteps = 50;
    int m_groupIndex = 0;
    int m_pendingToolCalls = 0;
    bool m_awaitingTools = false;
    bool m_exporting = false;          // 最后一组的导出回合已发起
    GroupPhase m_phase = GroupPhase::Build;
    QSet<QString> m_planning;          // 规划进行中的区（chat.cancel 可丢弃迟到结果）
    QDateTime m_startedAt;
    std::function<void(const QString&, bool)> m_toolCallHook;
};
```

> `setModelConfig` 由 CoreApp 在每次 `plan()/execute()` 前用主 agent 配置调用；`m_cfg` 缺省为空时 `LlmGateway` 会报 `agent-not-configured`（不会静默发出错误请求）。

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
    m_planning.insert(area);
    attemptPlan(0);
}

void AgentOrchestrator::attemptPlan(int attempt)
{
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    const QJsonArray messages =
        initialMessages(QString::fromUtf8(kPlanSystemPrompt), m_prompt);
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
            m_planning.remove(m_area);
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
                       textChunk("自检通过"),
                       textChunk("已导出 glb/fbx/blend")});
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    BuildPlan plan;
    plan.groups = {{"底座与环境", {"底座"}, "俯视截图检查底座完整"}};
    plan.maxSteps = 18;
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, {{/* execute_blender_code schema */}}, plan,
                 "做一个雨夜便利店", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QCOMPARE(llm.requestCount(), 4);                    // 建造 → 自检 → 导出 → 收尾
    QVERIFY(llm.requestAt(1).contains("验收标准"));      // 自检指令已注入
    QVERIFY(llm.requestAt(3).contains("export_scene")); // 导出指令已注入
}

void executorAdvancesGroups()
{
    MockLlmServer llm({toolCallChunk("execute_blender_code"), textChunk("组1建造完成"),
                       textChunk("组1自检通过"),
                       toolCallChunk("execute_blender_code"), textChunk("组2建造完成"),
                       textChunk("组2自检通过"),
                       textChunk("已导出 glb/fbx/blend")});
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

QJsonArray AgentOrchestrator::initialMessages(const QString& system, const QString& user) const
{
    QJsonArray messages{QJsonObject{{"role", "system"}, {"content", system}}};
    for (const QJsonValue& item : m_history)
        messages.append(item);                     // 规格 8.4：本区最近 20 条纯文本历史
    messages.append(QJsonObject{{"role", "user"}, {"content", user}});
    return messages;
}

void AgentOrchestrator::setConversationHistory(const QJsonArray& history)
{
    m_history = history;
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
    m_targetFolder = targetFolder;
    m_startedAt = QDateTime::currentDateTime();
    m_exporting = false;
    if (m_plan.groups.isEmpty()) {
        fail(QStringLiteral("empty-plan"));
        return;
    }
    m_messages = initialMessages(
        blenderSystemPrompt(),
        QStringLiteral("已确认的专业提示词：\n%1\n目标存放目录：%2。").arg(prompt, targetFolder));
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
    if (!m_exporting) {
        m_exporting = true;           // 最后一轮：按规格 5.5 导出三件套并保存源文件
        m_messages.append(QJsonObject{
            {"role", "user"},
            {"content", QStringLiteral(
                "全部组已完成。现在执行导出（execute_blender_code），基名沿用草稿命名清单：\n"
                "import bpy, os\n"
                "folder = r\"%1\"\n"
                "name = \"<草稿中的英文基名>\"\n"
                "bpy.ops.export_scene.gltf(filepath=os.path.join(folder, name + \".glb\"), export_format='GLB')\n"
                "bpy.ops.export_scene.fbx(filepath=os.path.join(folder, name + \".fbx\"))\n"
                "bpy.ops.wm.save_as_mainfile(filepath=os.path.join(folder, name + \".blend\"))\n"
                "三个文件必须同基名；导出完成后停止调用工具。").arg(m_targetFolder)}});
        m_stream.clear();
        tick();
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

void AgentOrchestrator::finishOk(const QString& text)
{
    const QStringList glbs = collectNewGlbs();
    if (!glbs.isEmpty())
        emit artifactsReady(m_area, glbs); // 先报产出，再收尾对话
    emit finalText(m_area, text);
    if (m_tm)
        m_tm->finish(m_taskId);           // 推进同区串行队列
    m_taskId.clear();
}

void AgentOrchestrator::finishCancelled()
{
    m_planning.remove(m_area);
    if (m_tm)
        m_tm->finish(m_taskId);           // TaskManager 已记录取消，finished(cancelled=true)
    m_taskId.clear();
}

void AgentOrchestrator::fail(const QString& reason)
{
    m_planning.remove(m_area);
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    if (m_taskId.isEmpty())
        return;
    const QString normalized = reason == QLatin1String("max steps exceeded")
                                   ? QStringLiteral("max-steps-exceeded")
                                   : reason;
    emit failed(m_area, normalized);
    if (m_tm)
        m_tm->finish(m_taskId);
    m_taskId.clear();
}

QStringList AgentOrchestrator::collectNewGlbs() const
{
    QStringList result;
    if (m_targetFolder.isEmpty())
        return result;
    const QDir dir(m_targetFolder);
    const QDateTime threshold = m_startedAt.addSecs(-2); // 容忍文件系统时间精度
    for (const QFileInfo& info : dir.entryInfoList({"*.glb"}, QDir::Files)) {
        if (info.lastModified() >= threshold)
            result << info.absoluteFilePath();
    }
    return result;
}
```

`executeToolCalls`（含工具调用钩子与截图回传）：

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
        if (m_toolCallHook)
            m_toolCallHook(name, true); // P6：异步工具调用全程抑制 watcher
        m_mcp->callTool(name, args, [this, call, name](const QJsonValue& result,
                                                       const QJsonObject& error) {
            if (m_toolCallHook)
                m_toolCallHook(name, false);
            emit toolStep(m_area, name, error.isEmpty() ? "done" : "error");
            QStringList images;
            m_messages.append(toolResultMessage(call.value("id").toString(), result, error,
                                                &images));
            for (const QString& dataUrl : images)
                emit imageReceived(m_area, m_taskId, dataUrl, name);
            if (--m_pendingToolCalls == 0) {
                m_awaitingTools = false;
                m_stream.clear();
                tick();               // 回喂后继续下一轮
            }
        });
    }
}
```

`toolResultMessage` 核心（截图为 image content 时压缩为 JPEG base64，同时收集给前端）：

```cpp
QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error, QStringList* images)
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
                if (jpeg.isEmpty())
                    continue;
                const QString dataUrl = QStringLiteral("data:image/jpeg;base64,")
                                        + QString::fromLatin1(jpeg.toBase64());
                content.append(QJsonObject{{"type", "image_url"},
                                           {"image_url", QJsonObject{{"url", dataUrl}}}});
                if (images)
                    images->append(dataUrl); // 同一份压缩结果：回喂模型 + 前端展示
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

CoreApp 创建 blender orchestrator（`m_blender` 在 Task 1 已就绪）；先在 `CoreApp.h` 补齐暂存结构：

```cpp
struct PendingPick {
    QString area;
    QString mode;          // lowpoly | cloud3d
    QString prompt;
    BuildPlan plan;
};

QHash<QString, QJsonObject> m_pendingPlans;    // area -> plan JSON
QHash<QString, QString> m_planPrompts;         // area -> 已确认提示词
QSet<QString> m_planCancelled;                 // area -> 规划取消标记
QHash<QString, PendingPick> m_pendingPicks;    // area -> 待选目录
QHash<QString, QString> m_pickRequests;        // requestId -> area
void openFolderPick(const QString& area);
```

接线（P6 `wireOrchestrator` 在此基础上统一，blender 与 unity 共用）：

```cpp
m_orch = new AgentOrchestrator(m_llm, m_blender->client(), this);
connect(m_orch, &AgentOrchestrator::toolStep, this,
        [this](const QString& area, const QString& tool, const QString& status) {
            m_ws->broadcastEvent("tool.step",
                                 {{"area", area}, {"tool", tool}, {"status", status}});
        });
connect(m_orch, &AgentOrchestrator::delta, this,
        [this](const QString& area, const QString& text) {
            m_ws->broadcastEvent("chat.delta", {{"area", area}, {"text", text}});
        });
connect(m_orch, &AgentOrchestrator::finalText, this,
        [this](const QString& area, const QString& text) {
            m_sessions->appendMessage(area, "assistant", text);
            m_ws->broadcastEvent("chat.message",
                                 {{"area", area}, {"role", "assistant"}, {"content", text}});
        });
connect(m_orch, &AgentOrchestrator::failed, this,
        [this](const QString& area, const QString& reason) {
            m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", reason}});
        });
connect(m_orch, &AgentOrchestrator::groupProgress, this,
        [this](const QString& area, int index, int count, const QString& title) {
            m_ws->broadcastEvent("task.progress",
                                 {{"area", area}, {"groupIndex", index},
                                  {"groupCount", count}, {"title", title}});
        });
connect(m_orch, &AgentOrchestrator::imageReceived, this,
        [this](const QString& area, const QString& taskId, const QString& dataUrl,
               const QString& caption) {
            m_ws->broadcastEvent("chat.image",
                                 {{"area", area}, {"taskId", taskId},
                                  {"dataUrl", dataUrl}, {"caption", caption}});
        });
connect(m_orch, &AgentOrchestrator::planReady, this,
        [this](const QString& area, const QString& taskId, const QJsonObject& plan) {
            if (m_planCancelled.remove(area))
                return;                       // 已取消的规划不再弹卡
            m_pendingPlans.insert(area, plan);
            m_ws->broadcastEvent("plan.created",
                                 {{"area", area}, {"taskId", taskId},
                                  {"groups", plan.value("groups")},
                                  {"totalSteps", plan.value("totalSteps")}});
        });
connect(m_orch, &AgentOrchestrator::artifactsReady, this,
        [this](const QString& area, const QStringList& absoluteGlbs) {
            if (!m_library || area != QLatin1String("blender"))
                return;
            QJsonArray changedDirs;
            for (const QString& abs : absoluteGlbs) {
                const QString relative = m_library->relativeFor(abs);
                if (relative.isEmpty())
                    continue;
                const QString dir = QFileInfo(relative).path();
                const QString base = QFileInfo(relative).completeBaseName();
                QJsonObject paths{{"glb", relative}};
                for (const QString& suffix : {QStringLiteral("fbx"),
                                              QStringLiteral("blend")}) {
                    const QString sibling = (dir == "." ? QString() : dir + '/')
                                            + base + '.' + suffix;
                    if (QFileInfo::exists(m_library->absoluteFor(sibling)))
                        paths.insert(suffix, sibling);
                }
                m_ws->broadcastEvent("asset.created",
                                     {{"area", area}, {"name", base},
                                      {"path", relative}, {"paths", paths}});
                changedDirs.append(dir);
            }
            if (!changedDirs.isEmpty())
                m_ws->broadcastEvent("fileTree.changed",
                                     {{"area", area}, {"reason", "incremental"},
                                      {"paths", changedDirs}});
        });
```

> 每次 `plan()/execute()` 之前调用 `m_orch->setModelConfig(primaryAgentConfig())` 与 `m_orch->setConversationHistory(m_sessions->recentMessages(area, 20))`；`m_orch` 加入 `CoreApp.h` 成员（`AgentOrchestrator* m_orch = nullptr;`），并 `#include "core/agent/AgentOrchestrator.h"`（`PendingPick` 持有 `BuildPlan` 需要完整类型）。P6 Task 3 把同一组 connect 封装为 `wireOrchestrator(orch)`（含 `artifactsReady`），blender 与 unity 共用，P6 落地时删除本段内联 connect。

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

`ChatPanel.vue` 新增订阅 `plan.created` → 追加计划卡并 `busy.value = false`（等待用户决策）；计划卡 `start` → `busy.value = true` 后 `client.request("plan.confirm", {area, taskId})`，`cancel` → `client.request("plan.cancel", {area, taskId})`；收到 `folderPick.open`（Task 4）后弹存放位置。

- [ ] **Step 6: CMake + 跑测试 + Commit**（`tst_orchestrator` 4 用例：规划解析 / 降级 / 单组执行 / 多组推进；`feat: Planner→Executor 两阶段主循环（计划卡/分组自检）`）

---

### Task 4: folderPick 流程 + 资产落盘

**Files:** Create `src/core/assets/AssetLibrary.h/.cpp`、`tests/tst_asset_library.cpp`、`web/src/components/FolderPicker.vue`；Modify `CoreApp`（`folderPick.submit/cancel/mkdir`、`fs.operate`、`tree.refresh`、`asset.import` 预留）、`CMakeLists.txt`。

**Interfaces:**
- 事件 `folderPick.open {area, requestId, rootName:"ModelLibrary", tree}`（tree 由 `AssetLibrary::folderTree()` 生成，`{name, path, children}`）。
- 请求 `folderPick.submit {requestId, relativePath}` → 校验（必须已存在、必须在根内）→ 调用 `orchestrator.execute(taskId, tm, tools, plan, prompt, targetFolder, area)`（plan/prompt 来自 `plan.confirm` 时缓存）。
- 请求 `folderPick.mkdir {requestId, path, name}` → 在弹窗内新建子文件夹，返回 `{ok, tree}` 供弹窗刷新；名称非法/重名返回 `invalid-path` / `already-exists`。
- 请求 `folderPick.cancel {requestId}` → 任务结束，广播 `error.card {reason:"user-cancelled"}`。
- 请求 `fs.operate {area, op, path, name?}` → `op ∈ mkdir|rename|delete|reveal|refresh`；仅 `area=="blender"` 由本任务实现（P6 接管 `unity` 的工程根）；操作成功后广播 `fileTree.changed {area:"blender", reason, paths}`（`mkdir/rename/delete` 为 `incremental` 且带受影响相对路径，`refresh` 为 `full` 且 `paths:[]`；`reveal` 不广播）；失败返回 `{ok:false, error}`（`invalid-path|already-exists|not-found|io-error|unknown-op`）。
- orchestrator 完成后（Task 3 `onTurnFinished` 的导出回合）：经 Blender MCP `execute_blender_code` 导出同基名的 `glb` + `fbx` 并 `save_as_mainfile` 保存 `blend`（规格 5.5）；广播 `asset.created {name, path:<相对资产库根的 glb 路径>, paths:{glb, fbx?, blend?}}`（存在的兄弟文件才收录），并由 `artifactsReady` 接线统一广播 `fileTree.changed {area:"blender", reason:"incremental", paths:[父目录]}`（Task 3 Step 4）。

- [ ] **Step 1: 实现 AssetLibrary 与 CoreApp handler**

```cpp
class AssetLibrary
{
public:
    explicit AssetLibrary(QString root);          // Documents/SynthForgeStudio/ModelLibrary
    QString root() const { return m_root; }
    QJsonObject folderTree() const;               // 递归目录树（仅目录）
    bool isValidTarget(const QString& relative) const; // canonical 前缀校验，防 ../ 逃逸
    QString absoluteFor(const QString& relative) const;
    QString relativeFor(const QString& absolute) const; // 越界或不存在返回空
    QStringList scanModels() const;               // glb + fbx，供文件树与资产选择器

    // 文件操作（fs.operate / folderPick.mkdir 后端）：返回错误码，空串=成功
    QString makeFolder(const QString& parentRelative, const QString& name);
    QString renameEntry(const QString& relative, const QString& newName);
    QString deleteEntry(const QString& relative);
    bool revealEntry(const QString& relative) const;

private:
    bool isInsideRoot(const QString& absolute) const;
    QString m_root;
};
```

`folderTree` 递归 `QDirIterator`（`QDir::Dirs | NoDotAndDotDot`），限制深度 6；`isValidTarget` / `isInsideRoot` 用 `QFileInfo(candidate).canonicalFilePath()` 前缀比对根目录，拒绝 `..`；`makeFolder/renameEntry/deleteEntry` 全部先过 `isInsideRoot`，禁止 `/` `\\` `..` 出现在名称中。

关键实现（其余成员同前）：

```cpp
bool AssetLibrary::isInsideRoot(const QString& absolute) const
{
    const QString root = QFileInfo(m_root).canonicalFilePath();
    const QString path = QFileInfo(absolute).canonicalFilePath();
    return !path.isEmpty() && (path == root || path.startsWith(root + '/'));
}

QString AssetLibrary::relativeFor(const QString& absolute) const
{
    const QString root = QFileInfo(m_root).canonicalFilePath();
    const QString path = QFileInfo(absolute).canonicalFilePath();
    if (path.isEmpty() || (path != root && !path.startsWith(root + '/')))
        return {};
    return QDir(root).relativeFilePath(path);
}

QStringList AssetLibrary::scanModels() const
{
    QStringList result;
    QDirIterator it(m_root, {"*.glb", "*.fbx"}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        result << QDir(m_root).relativeFilePath(it.next());   // 统一正斜杠相对路径
    return result;
}

QString AssetLibrary::makeFolder(const QString& parentRelative, const QString& name)
{
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/')
        || name.contains('\\'))
        return QStringLiteral("invalid-path");
    const QString parent = absoluteFor(parentRelative);
    if (!isInsideRoot(parent) || !QDir(parent).exists())
        return QStringLiteral("invalid-path");
    const QString target = QDir(parent).filePath(name);
    if (QFileInfo::exists(target))
        return QStringLiteral("already-exists");
    return QDir().mkpath(target) ? QString() : QStringLiteral("io-error");
}

QString AssetLibrary::renameEntry(const QString& relative, const QString& newName)
{
    if (newName.isEmpty() || newName.contains('/') || newName.contains('\\'))
        return QStringLiteral("invalid-path");
    const QString abs = absoluteFor(relative);
    if (!isInsideRoot(abs) || !QFileInfo::exists(abs))
        return QStringLiteral("not-found");
    const QString target = QFileInfo(abs).dir().filePath(newName);
    if (QFileInfo::exists(target))
        return QStringLiteral("already-exists");
    return QDir().rename(abs, target) ? QString() : QStringLiteral("io-error");
}

QString AssetLibrary::deleteEntry(const QString& relative)
{
    const QString abs = absoluteFor(relative);
    if (relative.isEmpty() || !isInsideRoot(abs) || !QFileInfo::exists(abs))
        return QStringLiteral("not-found");
    const QFileInfo info(abs);
    const bool ok = info.isDir() ? QDir(abs).removeRecursively() : QFile::remove(abs);
    return ok ? QString() : QStringLiteral("io-error");
}

bool AssetLibrary::revealEntry(const QString& relative) const
{
    const QString abs = absoluteFor(relative);
    if (!isInsideRoot(abs) || !QFileInfo::exists(abs))
        return false;
    return QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        {QStringLiteral("/select,"), QDir::toNativeSeparators(abs)});
}
```

`CoreApp` 的规划 → 选目录 → 执行链路（`draft.send` / `draft.cancel` / `plan.confirm` / `plan.cancel` / `folderPick.submit` / `folderPick.cancel` 全部在本任务落地；数据结构见 Task 3 Step 4）：

```cpp
m_router->registerHandler("draft.send", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    const QString prompt = params.value("prompt").toString().trimmed();
    const QString mode = params.value("mode").toString(QStringLiteral("lowpoly"));
    if (prompt.isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "empty-prompt"}});
    m_sessions->saveDraft(area, prompt);
    m_planPrompts.insert(area, prompt);          // plan.confirm 复用编辑后的提示词
    if (mode == "cloud3d") {                     // 云模式不规划：直接选目录（执行由 P5 接管）
        m_pendingPicks.insert(area, PendingPick{area, mode, prompt, BuildPlan{}});
        openFolderPick(area);
        return QJsonValue(QJsonObject{{"ok", true}});
    }
    m_planCancelled.remove(area);
    m_orch->setModelConfig(primaryAgentConfig());
    m_orch->setConversationHistory(m_sessions->recentMessages(area, 20));
    m_orch->plan(QStringLiteral("plan-%1").arg(++m_seq), nullptr, prompt, area);
    return QJsonValue(QJsonObject{{"ok", true}}); // 规划不占同区串行队列
});

m_router->registerHandler("draft.cancel", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    m_planCancelled.insert(area);                // 迟到的 planReady 不再弹卡
    m_pendingPlans.remove(area);
    m_pendingPicks.remove(area);
    return QJsonValue(QJsonObject{{"ok", true}}); // 用户主动放弃，不发 error.card
});

m_router->registerHandler("plan.confirm", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    const QJsonObject json = m_pendingPlans.take(area);
    if (json.isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "plan-not-found"}});
    BuildPlan plan;
    for (const QJsonValue& value : json.value("groups").toArray()) {
        const QJsonObject item = value.toObject();
        BuildGroup group;
        group.title = item.value("title").toString();
        for (const QJsonValue& name : item.value("items").toArray())
            group.items << name.toString();
        group.acceptance = item.value("acceptance").toString();
        plan.groups.append(group);
    }
    plan.maxSteps = json.value("totalSteps").toInt(50); // 规划阶段已按 8×组数+10 计算并封顶
    m_pendingPicks.insert(area, PendingPick{area, "lowpoly",
                                            m_planPrompts.value(area), plan});
    openFolderPick(area);
    return QJsonValue(QJsonObject{{"ok", true}});
});

m_router->registerHandler("plan.cancel", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    m_planCancelled.insert(area);
    m_pendingPlans.remove(area);
    m_pendingPicks.remove(area);
    m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", "user-cancelled"}});
    return QJsonValue(QJsonObject{{"ok", true}});
});

m_router->registerHandler("chat.cancel", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString();
    m_tasks->cancelArea(area);
    if (m_orch->isPlanning(area)) {          // 规划不在 TaskManager 队列，需单独丢结果
        m_planCancelled.insert(area);
        m_pendingPlans.remove(area);
    }
    return QJsonValue(QJsonObject{{"ok", true}});
});

m_router->registerHandler("folderPick.submit", [this](const QJsonObject& params) {
    const QString requestId = params.value("requestId").toString();
    const QString area = m_pickRequests.take(requestId);
    if (area.isEmpty() || !m_pendingPicks.contains(area))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "request-not-found"}});
    const QString relative = params.value("relativePath").toString();
    if (!m_library->isValidTarget(relative))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
    const PendingPick pick = m_pendingPicks.take(area);
    const QString targetFolder = m_library->absoluteFor(relative);
    if (pick.mode == "cloud3d") {
        // P4 阶段占位；P5 在此调用 Cloud3dService::start(area, taskId, prompt, targetFolder)
        m_ws->broadcastEvent("error.card",
                             {{"area", area}, {"reason", "cloud3d-not-implemented"}});
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "cloud3d-not-implemented"}});
    }
    const QString taskId = QStringLiteral("build-%1").arg(++m_seq);
    m_tasks->enqueue(
        Task{taskId, area, QString(),
             [this, pick, targetFolder](const QString& tid, TaskManager* tm) {
                 m_orch->setModelConfig(primaryAgentConfig());
                 m_orch->setConversationHistory(
                     m_sessions->recentMessages(pick.area, 20));
                 m_orch->execute(tid, tm, m_blenderTools, pick.plan,
                                 pick.prompt, targetFolder, pick.area);
             }});
    return QJsonValue(QJsonObject{{"ok", true}, {"taskId", taskId}});
});

m_router->registerHandler("folderPick.cancel", [this](const QJsonObject& params) {
    const QString area = m_pickRequests.take(params.value("requestId").toString());
    if (area.isEmpty())                 // 旧弹窗（requestId 已被新请求取代）不报错卡
        return QJsonValue(QJsonObject{{"ok", true}});
    m_pendingPicks.remove(area);
    m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", "user-cancelled"}});
    return QJsonValue(QJsonObject{{"ok", true}});
});

void CoreApp::openFolderPick(const QString& area)
{
    // 清掉该区未被消费的旧 requestId，旧弹窗若仍开着再提交会得到 request-not-found
    for (auto it = m_pickRequests.begin(); it != m_pickRequests.end();) {
        if (it.value() == area)
            it = m_pickRequests.erase(it);
        else
            ++it;
    }
    const QString requestId = QStringLiteral("pick-%1").arg(++m_seq);
    m_pickRequests.insert(requestId, area);
    m_ws->broadcastEvent("folderPick.open",
                         {{"area", area}, {"requestId", requestId},
                          {"rootName", "ModelLibrary"},
                          {"tree", m_library->folderTree()}});
}
```

> `folderPick.submit` 把 TaskManager 回调参数 `tid` 传给 `execute()`（与入队 id 相同，但以回调参数为准）；此后由 `execute()` 内部 `finishOk/finishCancelled/fail` 调用 `TaskManager::finish` 推进队列。`plan.maxSteps` 直接采用 planReady 的 `totalSteps`（已在规划阶段算好并封顶 60）。

`fs.operate` 与 `folderPick.mkdir` handler：

```cpp
m_router->registerHandler("fs.operate", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    if (area != "blender") // Unity 区由 P6 接管（工程根路径）
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "area-not-supported"}});
    const QString op = params.value("op").toString();
    const QString path = params.value("path").toString();
    const QString name = params.value("name").toString();
    QString err;
    if (op == "mkdir")
        err = m_library->makeFolder(path, name);
    else if (op == "rename")
        err = m_library->renameEntry(path, name);
    else if (op == "delete")
        err = m_library->deleteEntry(path);
    else if (op == "reveal") {
        if (!m_library->revealEntry(path))
            err = QStringLiteral("not-found");
    } else if (op != "refresh") {
        err = QStringLiteral("unknown-op");
    }
    if (!err.isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", err}});
    if (op != "reveal") // 打开资源管理器不改变树
        m_ws->broadcastEvent("fileTree.changed",
                             {{"area", "blender"},
                              {"reason", op == "refresh" ? "full" : "incremental"},
                              {"paths", op == "refresh" ? QJsonArray{}
                                                        : QJsonArray{path}}});
    return QJsonValue(QJsonObject{{"ok", true}});
});

m_router->registerHandler("folderPick.mkdir", [this](const QJsonObject& params) {
    const QString err = m_library->makeFolder(params.value("path").toString(),
                                              params.value("name").toString());
    if (!err.isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", err}});
    return QJsonValue(QJsonObject{{"ok", true}, {"tree", m_library->folderTree()}});
});
```

- [ ] **Step 2: 文件操作测试**

`tests/tst_asset_library.cpp`：

```cpp
#include <QTemporaryDir>
#include <QtTest>

#include "core/assets/AssetLibrary.h"

class AssetLibraryTest : public QObject
{
    Q_OBJECT
private slots:
    void folderOpsStayInsideRoot()
    {
        QTemporaryDir dir;
        AssetLibrary lib(dir.path());
        QCOMPARE(lib.makeFolder("", "Props"), QString());
        QVERIFY(QDir(dir.path() + "/Props").exists());
        QCOMPARE(lib.makeFolder("", "Props"), QStringLiteral("already-exists"));
        QCOMPARE(lib.makeFolder("..", "Escape"), QStringLiteral("invalid-path"));
        QCOMPARE(lib.makeFolder("", "../Escape"), QStringLiteral("invalid-path"));

        QCOMPARE(lib.renameEntry("Props", "Props2"), QString());
        QVERIFY(QDir(dir.path() + "/Props2").exists());
        QCOMPARE(lib.deleteEntry("Props2"), QString());
        QVERIFY(!QDir(dir.path() + "/Props2").exists());
        QCOMPARE(lib.deleteEntry("Props2"), QStringLiteral("not-found"));
    }

    void validTargetRejectsEscape()
    {
        QTemporaryDir dir;
        AssetLibrary lib(dir.path());
        QVERIFY(lib.isValidTarget(""));
        QVERIFY(!lib.isValidTarget("../outside"));
    }
};

QTEST_MAIN(AssetLibraryTest)
#include "tst_asset_library.moc"
```

CMake 追加 `tst_asset_library`（链接 `synthforge_core` + `Qt6::Test`）。
Run: `ctest --test-dir build -C Debug -R tst_asset_library --output-on-failure`
Expected: 2 用例 Passed。

- [ ] **Step 3: 前端 FolderPicker.vue**

弹窗树（缩进列表 + 单选），顶部 `[新建文件夹]`（`window.prompt` 名称 → `folderPick.mkdir {requestId, path: selectedPath, name}`，用返回的 `tree` 刷新），底部 `[取消] [确认]`；确认发 `folderPick.submit {requestId, relativePath: 选中相对路径}`。props：`{client, area, requestId, tree}`（由 ChatPanel 在 `folderPick.open` 时传入；只渲染 `area` 匹配的弹窗）。样式复用设置弹窗的遮罩类。

- [ ] **Step 4: 手工验证**（真实 Blender：发送需求 → 草稿 → 发送 → 计划卡确认 → 选子文件夹（先新建一个）→ 模型按组生成并落盘 → `asset.created` 触发文件树刷新；右键重命名/删除后树刷新）

- [ ] **Step 5: Commit**（`feat: 存放位置确认框与 GLB 资产落盘；资产库文件操作（fs.operate）`）

---

### Task 5: 文件树数据源 + three.js GLB 预览

**Files:** Modify `web/src/components/FileTree.vue`、`PreviewPane.vue`、`App.vue`；Modify `CoreApp`（`assets.list`、`file.read`、`fileTree.changed`）。

**Interfaces:**
- 请求 `assets.list {}` → `[{name, relativePath, kind}]`（`kind ∈ glb|fbx`；目录由前端按相对路径聚合，FileTree 仅列 `kind==="glb"`，P6 AssetPicker 取 `glb`+`fbx`）；事件 `fileTree.changed {area:"blender", reason, paths}` 触发重新拉取（可直接全量刷新，`paths` 仅作后续优化）。
- 请求 `file.read {area, path}` → `{ok, kind:'text'|'image'|'model', content?, dataUrl?, language?}`；blender 区路径相对资产库根（复用 `isValidTarget`）；unity 区由 P6 接管（P4 返回 `area-not-supported`）。
- `PreviewPane` props 与 P3 Task 6 一致：`{area, file}`；`file.kind==='model'` 时用 three.js 加载 `file.dataUrl`；未选中 → 概览卡。
- `App.vue`：FileTree `select(path)` → `file.read` → `previewFile`；订阅 `asset.created` → 用其相对路径 `path` 自动加载新资产。

`file.read` handler（CoreApp）：

```cpp
m_router->registerHandler("file.read", [this](const QJsonObject& params) {
    const QString area = params.value("area").toString();
    const QString path = params.value("path").toString();
    if (area != "blender") // P6 接管 unity 工程内文件
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "area-not-supported"}});
    if (!m_library->isValidTarget(path))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
    const QString abs = m_library->absoluteFor(path);
    const QFileInfo info(abs);
    if (!info.exists() || info.isDir())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "not-found"}});
    const QString suffix = info.suffix().toLower();
    if (QStringList{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                    QStringLiteral("webp")}.contains(suffix)) {
        QFile file(abs);
        if (!file.open(QIODevice::ReadOnly))
            return QJsonValue(QJsonObject{{"ok", false}, {"error", "io-error"}});
        const QString mime = suffix == "png" ? QStringLiteral("image/png")
                                             : QStringLiteral("image/jpeg");
        return QJsonValue(QJsonObject{
            {"ok", true}, {"kind", "image"},
            {"dataUrl", QStringLiteral("data:%1;base64,").arg(mime)
                            + QString::fromLatin1(file.readAll().toBase64())}});
    }
    if (suffix == "glb") {
        return QJsonValue(QJsonObject{{"ok", true}, {"kind", "model"},
                                      {"dataUrl", QUrl::fromLocalFile(abs).toString()}});
    }
    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "io-error"}});
    if (file.size() > 2 * 1024 * 1024)
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "too-large"}});
    const QString language = suffix == "cs" ? QStringLiteral("csharp")
                            : suffix == "lua" ? QStringLiteral("lua")
                            : suffix == "json" ? QStringLiteral("json")
                            : QStringLiteral("typescript");
    return QJsonValue(QJsonObject{{"ok", true}, {"kind", "text"},
                                  {"content", QString::fromUtf8(file.readAll())},
                                  {"language", language}});
});
```

`assets.list` handler（文件树与 AssetPicker 的唯一数据源；`kind` 为不含点后缀，返回 glb 与 fbx；目录由前端按相对路径聚合）：

```cpp
m_router->registerHandler("assets.list", [this](const QJsonObject&) {
    QJsonArray items;
    for (const QString& relative : m_library->scanModels()) {
        const QFileInfo info(relative);
        items.append(QJsonObject{{"name", info.baseName()},
                                 {"relativePath", relative},
                                 {"kind", info.suffix().toLower()}});
    }
    return QJsonValue(items);
});
```

> 若 WebEngine 拒绝 `file:///`（自定义 scheme 页面跨 local 资源），把 glb 分支改为 `data:application/octet-stream;base64,` + 文件字节（低模 GLB 通常 < 5MB，内存可接受）。

- [ ] **Step 1: 实现 PreviewPane 核心**

```vue
<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import hljs from "highlight.js/lib/core";
import csharp from "highlight.js/lib/languages/csharp";
import json from "highlight.js/lib/languages/json";
import typescript from "highlight.js/lib/languages/typescript";
import lua from "highlight.js/lib/languages/lua";
import "highlight.js/styles/vs2015.css";

hljs.registerLanguage("csharp", csharp);
hljs.registerLanguage("json", json);
hljs.registerLanguage("typescript", typescript);
hljs.registerLanguage("lua", lua);

interface PreviewFile {
  path: string;
  kind: "text" | "image" | "info" | "model";
  content?: string;
  dataUrl?: string;
  language?: string;
}

const props = defineProps<{ area: "blender" | "unity"; file: PreviewFile | null }>();
const host = ref<HTMLDivElement>();
const isModel = computed(() => props.file?.kind === "model");
const loadError = ref("");

let renderer: THREE.WebGLRenderer;
let scene: THREE.Scene;
let camera: THREE.PerspectiveCamera;
let controls: OrbitControls;
let loaded: THREE.Group | null = null;
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
  const loop = () => {
    frame = requestAnimationFrame(loop);
    controls.update();
    renderer.render(scene, camera);
  };
  loop();
});

watch(() => props.file, (f) => {
  loadError.value = "";
  if (loaded) {
    scene.remove(loaded);
    loaded = null;
  }
  if (!f || f.kind !== "model" || !f.dataUrl)
    return;
  new GLTFLoader().load(
    f.dataUrl,
    (gltf) => {
      loaded = gltf.scene;
      scene.add(loaded);
    },
    undefined,
    () => { loadError.value = "模型加载失败"; },
  );
});

onBeforeUnmount(() => {
  cancelAnimationFrame(frame);
  window.removeEventListener("resize", resize);
  renderer.dispose();
});

function highlighted(content: string, language?: string) {
  const lang = language && hljs.getLanguage(language) ? language : "typescript";
  return hljs.highlight(content, { language: lang }).value;
}
</script>

<template>
  <section class="preview">
    <div ref="host" class="canvas-host" :class="{ hidden: !isModel }" />
    <p v-if="isModel && loadError" class="info-card">{{ loadError }}</p>
    <template v-else-if="!isModel && file">
      <img v-if="file.kind === 'image'" :src="file.dataUrl" class="preview-image" :alt="file.path" />
      <pre v-else-if="file.kind === 'text'" class="code"><code v-html="highlighted(file.content ?? '', file.language)" /></pre>
      <div v-else class="info-card">
        <h3>{{ file.path }}</h3>
        <p>暂无可用预览</p>
      </div>
    </template>
    <div v-else-if="!isModel" class="info-card">
      <h3>{{ area === "blender" ? "资产库概览" : "工程信息" }}</h3>
      <p>{{ area === "blender" ? "选中资产后在此预览" : "选中文件后在此预览" }}</p>
    </div>
  </section>
</template>

<style scoped>
.preview { flex: 1; min-width: 0; position: relative; background: #1e1e1e; }
.canvas-host { position: absolute; inset: 0; }
.canvas-host.hidden { display: none; }
.preview-image { max-width: 100%; max-height: 100%; margin: auto; object-fit: contain; }
.code { position: absolute; inset: 0; overflow: auto; margin: 0; padding: 12px; }
.info-card { margin: auto; text-align: center; color: #999; }
</style>
```

依赖增量：`web/package.json` 加 `"three": "^0.169.0"`、`"@types/three": "^0.169.0"`（`highlight.js` 已在 P3 Task 6 安装）。

`FileTree.vue`：在 P3 Task 6 的右键菜单骨架上填数据：`assets.list` 拉取 → 按 `kind === "glb"` 过滤出文件项 → 文件夹可折叠 + `.glb` 文件项（`.blend`/`.fbx` 仅作为资产卡引用不单独列出，规格 4.4；fbx 仅供 P6 AssetPicker 消费）；点击文件 emit `select(relativePath)`；`fileTree.changed` → 重新拉取；右键节点的 `path` 使用相对路径。

`App.vue` 接线（在 P3 Task 6 的 `previewFile` 基础上）：

```ts
async function openPreview(path: string) {
  const res: any = await client.request("file.read", { area: active.value, path });
  if (res.ok)
    previewFile.value = { path, kind: res.kind, content: res.content,
                          dataUrl: res.dataUrl, language: res.language };
}

onMounted(() => {
  client.on("asset.created", async (d: any) => {
    if (active.value !== "blender" || !d.path)
      return;
    await openPreview(d.path); // 生成完成自动加载新资产（规格 4.5）
  });
});
```

> 同时把 P3 `App.vue` 的 `previewFile` 类型扩为 `"text" | "image" | "info" | "model"`，与 P4 `PreviewFile` 保持一致（`QUrl` 需在 `CoreApp.cpp` 顶部 `#include <QUrl>`）。

`<FileTree @select="openPreview" ... />`、`<ChatPanel ... :selected-asset="..." />`（Task 6）。

- [ ] **Step 2: 手工验收（P4 总验收）**

1. 真实链路：Blender 打开且插件运行；发送「做三个低模木箱」→ 草稿卡出现；双击可编辑；发送 → 计划卡出现（分组/验收标准可读）→ [开始生成] → 选 `Props/` 子目录 → 步骤卡逐步更新 + `task.progress` 显示当前组 → 对话区出现截图 → 完成后文件树出现 `Props/xxx.glb`，点击可预览旋转。
2. 拒绝路径：关掉 Blender 插件 → 发送 → 立刻出现 `blender-not-ready` 错误卡，无总结 agent 请求。

- [ ] **Step 3: Commit**（`feat: 资产文件树与 three.js GLB 预览`）

---

### Task 6: 修改已有资产（跳过草稿/计划）

**Files:** Modify `CoreApp`（`chat.send` 增加 `mode:"modify"` + `asset` 参数）、`web/src/components/ChatPanel.vue`（目标资产 chip）、`App.vue`（FileTree 选中项传给 ChatPanel）；无新 C++ 模块（复用 `AgentOrchestrator::execute`）。

**Interfaces:**
- 前端：FileTree 选中 `.glb` → ChatPanel 输入框上方显示 `目标资产：<name> ✕` chip；发送 `chat.send {area:"blender", mode:"modify", asset:"道具/sword_01/sword_01.glb", text:"把剑改短一点"}`。
- CoreApp：`mode=="modify"` → `checkLowpoly()` → 校验 `asset` 在库内且存在（否则返回 `{"accepted":false, "error":"asset-not-found"}`，不占 error.card 字典）→ 构造单组计划并调 `execute()`，**不经过** SummarizerAgent、plan 卡、folderPick。
- prompt 包装：`"打开并修改已有资产 <绝对路径>。要求：<用户原话>。完成后覆盖导出同名 GLB/FBX 并保存 .blend。"`；`targetFolder = 资产所在目录`。
- 完成后 `asset.created` 同名覆盖并触发 `fileTree.changed`。

- [ ] **Step 1: CoreApp 接线**

在 `chat.send` 的 Task 1 前置检查之后、lowpoly 总结 agent 分支之前插入：

```cpp
if (mode == "modify") {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    const QString asset = params.value("asset").toString();
    const QString text = params.value("text").toString();
    const QFileInfo info(m_library->absoluteFor(asset));
    if (asset.isEmpty() || !info.exists()
        || !m_library->isValidTarget(QFileInfo(asset).path()))
        return QJsonValue(QJsonObject{{"accepted", false}, {"error", "asset-not-found"}});
    if (appendUser) {
        m_sessions->appendMessage(area, "user", text);
        m_ws->broadcastEvent("chat.message",
                             {{"area", area}, {"role", "user"}, {"content", text}});
    }
    BuildPlan plan;
    plan.groups.append(
        BuildGroup{QStringLiteral("修改资产"), {},
                   QStringLiteral("修改意图达成；同名 GLB/FBX/blend 已覆盖")});
    plan.maxSteps = 50;
    const QString prompt = QStringLiteral("打开并修改已有资产 %1。要求：%2。"
                                          "完成后覆盖导出同名 GLB/FBX 并保存 .blend。")
                               .arg(info.absoluteFilePath(), text);
    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);
    const QString targetFolder = info.absolutePath();
    m_tasks->enqueue(
        Task{taskId, area, QString(),
             [this, plan, prompt, targetFolder, area](const QString& tid, TaskManager* tm) {
                 m_orch->setModelConfig(primaryAgentConfig());
                 m_orch->setConversationHistory(m_sessions->recentMessages(area, 20));
                 m_orch->execute(tid, tm, m_blenderTools, plan, prompt, targetFolder, area);
             }});
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}
```

- [ ] **Step 2: ChatPanel 目标资产 chip**

```ts
const props = defineProps<{
  area: "blender" | "unity";
  client: WsClient;
  selectedAsset?: string;
}>();

async function send() {
  const text = input.value.trim();
  if (!text) return;
  input.value = "";
  const modify = props.area === "blender" && !!props.selectedAsset;
  const res: any = await props.client.request("chat.send", {
    area: props.area,
    // 选中资产 → modify；否则 blender 区用顶部模式切换（lowpoly/cloud3d）
    mode: modify ? "modify" : props.area === "blender" ? mode.value : undefined,
    text,
    asset: modify ? props.selectedAsset : undefined,
  });
  busy.value = res?.accepted !== false; // 被前置检查拒绝时不进入“进行中”
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
- [ ] 截图压缩后回喂（长边 ≤1024、JPEG q80），且同一截图以 `chat.image` 事件在对话区渲染为图片卡
- [ ] 取消任务后 MCP 调用在工具边界停止，任务事件 `finished(cancelled=true)`；草稿/规划/计划/执行四个可等待阶段的「停止」均生效（规划丢弃迟到结果不弹卡）
- [ ] `tst_preflight`、`tst_mcp_runtime`、`tst_asset_library`、`tst_orchestrator` 全绿
- [ ] `fs.operate` 五项操作全部可用（mkdir/rename/delete/reveal/refresh），越界路径返回 `invalid-path`；`folderPick.mkdir` 可在弹窗内新建文件夹
- [ ] `file.read` 支持文本/图片/GLB 三类；`asset.created` 后预览区自动加载新资产；未选中时显示概览卡
