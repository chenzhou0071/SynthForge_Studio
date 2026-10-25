# SynthForge Studio P4：低模建模全链实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** blender 区低模模式完整闭环：发送前置检查 → 总结 agent 生成专业提示词草稿（可编辑/发送）→ 规划调用生成建造计划卡（分组/清单/验收标准，可确认）→ 存放位置确认框 → 分组执行主循环（建造→自检，工具调用经 Blender MCP，截图回传对话区）→ 导出 GLB 落盘 → 文件树更新 → three.js 预览。

**Architecture:** `PreflightChecker` 在 chat.send / draft.send 入口做拒绝式检查；`SummarizerAgent` 用独立 system prompt 生成草稿并广播 `draft.created`；`AgentOrchestrator` 分两阶段——`plan()` 一次纯文本调用产出分组计划（PlanCard 确认），`execute()` 按组「建造 → 自检」状态机驱动 LLM tool_calls，工具调用经 P1 `McpClient`；截图压缩后以 base64 回喂模型并广播前端；落盘路径由 `folderPick` 确认后执行 `bpy.ops.export_scene.gltf`。

**规格映射：** 5.2 发送链路与总结 agent、5.3 建造计划确认、5.4 存放位置确认框、5.6 低模执行链（分组自检）、5.5 落盘与预览、4.4 预览面板、9 错误处理。

**依赖：** P2（LlmGateway / TaskManager / WS）、P3（聊天面板 / 文件树 / 预览骨架）。

## Global Constraints

- **前置检查先于总结 agent**：云模式未配置 / Blender 链未就绪 → 直接 `error.card`（`reason` 分别为 `cloud3d-not-configured` / `blender-not-ready`），不浪费改写调用。（**Task 7 起修订**：低模 / 修改改为草稿阶段不预检，`draft.send` 时才检查；云模式仍早拦。）
- 总结 agent 与主 agent 配置相互独立；总结 agent 未配置时回退主 agent 配置。
- 工具清单按区裁剪：blender 区仅 Blender MCP 工具 + 文件工具。
- 截图回传模型前统一压缩：最长边 ≤1024px，JPEG q80，base64。
- 资产库根固定：`文档/SynthForgeStudio/ModelLibrary`（QStandardPaths::DocumentsLocation）；存放位置只能在该根的子路径中选择。
- 落盘后 GLB 与 .blend 一并保留；命名遵循草稿中的英文命名清单。
- **低模新资产走 Planner→Executor**：草稿发送后先规划、计划确认后才选存放位置并执行；解析失败降级默认四分组，不阻塞。修改已有资产不经过规划。
- 步数预算按计划动态计算：`maxSteps = min(8 × 组数 + 10, 60)`。
- **LLM 测试必须走真实 HTTP + SSE**：禁止 `class Xxx : public LlmGateway` 重写 `chatStream` 来造 mock，也不允许把 `chatStream` 改成可注入回调来绕过网络。所有 LLM 相关测试一律让真实 `chatStream` 发出 `POST /chat/completions`，由 `MockLlmServer`（`QTcpServer` 假服务端）在 TCP 上回真实的 chunked SSE 字节。判据：删掉 `LlmGateway.cpp` 里 `readyRead` / `finished` 任一分支，测试必须变红；若仍绿，说明断言盯错了对象。

## File Structure

```
src/core/mcp/McpRuntime.h/.cpp               ← 桥接运行时（工具缓存/重连状态/主动重启，Task 0）
src/core/mcp/BlenderGate.h/.cpp              ← Blender 会话互斥租约（单 owner，Task 2B）
src/core/preflight/PreflightChecker.h/.cpp   ← Blender/云链就绪检查
src/core/agent/SummarizerAgent.h/.cpp        ← 草稿生成（draft.created/draft.send）
src/core/agent/AgentOrchestrator.h/.cpp      ← plan() 规划调用 + execute() 分组执行状态机 + 截图压缩 + 取消
src/core/assets/AssetLibrary.h/.cpp          ← 资产库根扫描/落盘/导出 GLB 编排
src/resources/prompts/summarizer.md          ← 总结 agent system prompt（骨架来自规格附录）
web/src/components/DraftCard.vue             ← 草稿卡（双击编辑 + 发送）
web/src/components/PlanCard.vue              ← 建造计划卡（布局约束/分组/清单/摆放/验收标准 + 开始/取消）
web/src/components/ChatPanel.vue             ← Modify：draft/plan 消息卡 + 修改资产目标 chip（Task 6）
web/src/components/FolderPicker.vue          ← 存放位置确认弹窗
web/src/components/PreviewPane.vue           ← three.js GLB 预览（升级 Task 5）
tests/tst_preflight.cpp
tests/mock/MockLlmServer.h/.cpp              ← 本地假 LLM 服务端（HTTP/1.1 chunked SSE，Task 1B）
tests/tst_llm_gateway.cpp                    ← 流式专项：delta 顺序/工具分片/尾帧/错误/并发/abort（Task 1B）
tests/tst_mcp_runtime.cpp                    ← mock server 握手/工具缓存/重启
tests/tst_blender_gate.cpp                   ← 门禁获取/拒绝抢占/非 owner 释放无效
tests/tst_asset_library.cpp                  ← 资产库文件操作与越界校验
tests/tst_orchestrator.cpp                   ← mock MCP + 本地假 LLM（真实 HTTP+SSE）集成
```

---

### Task 0: McpRuntime（桥接运行时：握手 / 工具缓存 / 重连状态 / 主动重启）

> **为何存在**：P1 只提供传输与客户端原语，P2 未组装 MCP 运行时。本任务把「传输 + 客户端 + 工具清单缓存 + 重连状态」封成一个可复用类；P4 的 `blender` 与 P6 的 `unity` 各持一个实例，`blenderTools` 的唯一来源就是它。

**Files:** Create `src/core/mcp/McpRuntime.h/.cpp`、`tests/tst_mcp_runtime.cpp`；Modify `CMakeLists.txt`。

**Interfaces:**
- Consumes: `McpClient`（P1 Task 3/6）、`McpTransport` / `StdioTransport`（P1 Task 4）、`mock_mcp_server`（P1 Task 4）。
- Produces:
  - `class McpRuntime : QObject`；构造 `McpRuntime(area, std::unique_ptr<McpTransport>, QObject* parent = nullptr)`，`area ∈ {"blender","unity"}`。
  - `bool start()`（`client->start()`：启动传输并握手）、`bool restart()`（`client->shutdown()` + `prepareRestart()` + `client->start()`，供 `bridge.restart` 使用）、`void stop()`（`client->shutdown()`，供切换工程断链；需要时再 `start()`）。
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
    m_client->start(); // 内部启动传输，started 后自动握手
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
    m_client->start();          // 重新启动传输；started → 自动重新握手
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

### Task 1B: LlmGateway 流式专项测试（真实 HTTP + SSE）

> **为何单独成任务**：P2 Task 6 的 `tst_headless_ws` 只验证「门开了、话能通、库能写」，`chat.send` 走的是未配置分支，从未触达 `LlmGateway` 的网络与聚合代码；P2 Task 5 只测了 `SseParser` 的拆帧。本任务把流式那 150 行真正跑起来，并把 `MockLlmServer` 做成后续 Task 2 / Task 3 共用的测试基础设施。
>
> **硬约束（不可协商）**：
> - **禁止** `class Xxx : public LlmGateway` 重写 `chatStream` 造 mock；也**禁止**把 `chatStream` 改成可注入回调 / 模板参数来绕过网络。测试必须让真实的 `chatStream` 发出 `POST /chat/completions`，由 `MockLlmServer` 在 TCP 上回 chunked SSE 字节。
> - 有效性的唯一判据：**删掉 `LlmGateway.cpp` 的 `readyRead` 或 `finished` 分支，本文件的用例必须变红**。若删了还绿，说明断言测的是假对象，重写用例。
> - 用例断言的是**客户端行为**（回调次数/顺序/内容），不锁死错误文案、不依赖真实 API Key、不访问外网。

**Files:**
- Create: `tests/mock/MockLlmServer.h/.cpp`（`QTcpServer` 假 LLM 服务端，不是 `LlmGateway` 子类）
- Create: `tests/tst_llm_gateway.cpp`
- Modify: `CMakeLists.txt`（新增 `tst_llm_gateway` target，`tests` 加入 include 目录）
- Modify: `src/core/llm/LlmGateway.h`（删掉 `chatStream` 的 `virtual`，让子类 mock 在编译期就不可能）

**Interfaces:**

```cpp
namespace MockLlm {
// 构造 OpenAI 流式 wire format 的 SSE 帧（只造字节，不参与解析）
QByteArray textChunk(const QString& text);        // choices[0].delta.content
QByteArray toolCallStartChunk(int index, const QString& id,
                              const QString& name, const QString& argsFragment);
QByteArray toolArgsChunk(int index, const QString& argsFragment); // 只带 index + arguments 续片
QByteArray toolCallChunk(const QString& name, const QString& args = "{}"); // 单帧完整调用
QByteArray doneChunk();                           // "data: [DONE]\n\n"
} // namespace MockLlm

class MockLlmServer : public QObject
{
    Q_OBJECT
public:
    struct Turn
    {
        QList<QByteArray> frames;      // 每个元素是一整帧（可带也可不带结尾空行）
        int splitEvery = 3;            // 每帧再切成 N 字节一片经 TCP 发送；0 = 整帧
        int frameDelayMs = 2;          // 片与片之间延迟，确保 readyRead 分多次到达
        int status = 200;              // != 200：回错误 JSON，不发 SSE
        QByteArray errorBody = R"({"error":{"message":"mock error"}})";
        bool stallAfterFrames = false; // 发完 frames 保持连接（abort 用例）
    };

    explicit MockLlmServer(QList<Turn> script, QObject* parent = nullptr);
    bool listen();                     // 127.0.0.1，端口 0 由系统分配
    QString baseUrl() const;           // "http://127.0.0.1:<port>/v1"（不带尾斜杠）
    LlmConfig config(const QString& model = "test-model") const;
                                       // {baseUrl(), "test-key", model}，直接喂 setModelConfig
    int requestCount() const;
    QString pathAt(int index) const;              // 断言 "/v1/chat/completions"
    QByteArray authorizationAt(int index) const;  // 断言 "Bearer test-key"
    QString requestAt(int index) const;           // compact JSON 文本（.contains 断注入内容）
    QJsonObject requestObjectAt(int index) const; // 需要按字段断言时用
};
```

- Consumes：P2 `LlmGateway`（`chatStream` / `StreamHandlers` / `LlmConfig`）、`SseParser`。
- Produces：`MockLlmServer` + `textChunk` / `toolCallChunk` 等构造器，供 Task 2（草稿流）与 Task 3（规划/执行状态机）复用。

**用例清单（7 个，全部写在 `tst_llm_gateway.cpp`）**

| # | 用例 | 假服务端造的场景 | 断言（盯的是生产代码哪一段） |
|---|---|---|---|
| 1 | `emitsDeltasInOrderThenDone` | 两帧文本 + `[DONE]`，每帧再切 5 字节一片 | `onDelta` 恰好两次且顺序为 好→的；`onDone` 恰好 1；`onError` 0；请求体 `stream==true`、`model`、`Authorization`、路径全对（`LlmGateway` 71-129） |
| 2 | `mergesToolCallFragmentsByIndex` | 两个 tool_call，index 1 先到、index 0 后到；`arguments` 各拆两段 | `onToolCalls` 只回调 1 次、2 个调用、按 index 升序、`arguments` 已解析成对象、`id` 保留；`onDelta` 为空（108-123 聚合 + 33-52 收口） |
| 3 | `completesWhenFinalFrameHasNoBlankLine` | 最后一帧不带 `\n\n`，随后 chunked 正常结束 | delta 收到、`onDone` 1；覆盖 `finished` 里补 `feed("\n\n")` 的兜底（136-140），否则任务会永远卡在运行中 |
| 4 | `reportsErrorOnceOnHttpError` | 401 + JSON 错误体，不发任何 SSE | `onError` 1、`onDone` 0、`onDelta` 0；只发 1 次请求（不自动重试）（133-135） |
| 5 | `reportsErrorWhenConnectionRefused` | 先 `listen` 拿端口再 `close`，保证端口无人监听 | `onError` 1、`onDone` 0；不锁错误文案（`QNetworkReply` 错误分支） |
| 6 | `keepsConcurrentStreamsSeparate` | 两个服务端用不同 `frameDelayMs` 交替到帧，共用一个 gateway | 两条流各自 delta 不串字、顺序不乱，各自 `onDone` 1（`m_replies` 与 per-reply `StreamState`） |
| 7 | `abortEndsInFlightStreamsOnce` | 发一帧后 `stallAfterFrames` 挂住不结束 | `abort()` 后该流 `onError` 1、`onDone` 0；重复 `abort()` 不再回调（145-150，`chat.cancel` 依赖此语义） |

> P2 Task 7 已实现按区中止：`chatStream(..., tag=area)` + `abortTag(area)`（每条流登记 reply→tag，只掐匹配项）。标签隔离由 `tst_headless_ws::cancelEndsOnlyTargetArea` 覆盖（把 `abortTag` 换成全局 `abort()`，该用例会红在"另一区仍连接"）；本表用例 7 继续验证全局 `abort()` 的"只回调一次"兜底语义。P4 编排器按区取消必须用 `abortTag(area)`，禁止 `abort()`。

- [ ] **Step 1: 写失败测试（红）**

```cpp
#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>
#include <QtTest>

#include "core/llm/LlmGateway.h"
#include "mock/MockLlmServer.h"

using namespace MockLlm;

// 约束：本文件禁止出现 `class Xxx : public LlmGateway`，也不允许回调注入绕过网络。
// 每个用例都必须让真实 chatStream 发出 POST /v1/chat/completions，
// 由 MockLlmServer 在 TCP 上回 chunked SSE。
// 有效性判据：删掉 LlmGateway.cpp 的 readyRead/finished 任一分支，这里必须变红。
class LlmGatewayTest : public QObject
{
    Q_OBJECT

private:
    struct Recorder
    {
        QStringList deltas;
        QJsonArray toolCalls;
        QStringList errors;
        int done = 0;

        LlmGateway::StreamHandlers handlers()
        {
            return {.onDelta = [this](const QString& d) { deltas << d; },
                    .onToolCalls = [this](const QJsonArray& calls) { toolCalls = calls; },
                    .onError = [this](const QString& e) { errors << e; },
                    .onDone = [this]() { ++done; }};
        }
    };

    static QJsonArray userMessages(const QString& text)
    {
        return QJsonArray{QJsonObject{{"role", "user"}, {"content", text}}};
    }

private slots:
    void emitsDeltasInOrderThenDone()
    {
        MockLlmServer server({Turn{.frames = {textChunk("好"), textChunk("的"), doneChunk()},
                                   .splitEvery = 5}}); // 切点故意落在 data:/JSON/UTF-8 中间
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("你好"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.deltas, (QStringList{"好", "的"})); // 不能少、不能并成一次、不能反序
        QVERIFY(rec.errors.isEmpty());
        QCOMPARE(server.pathAt(0), QStringLiteral("/v1/chat/completions")); // baseUrl 尾斜杠已裁掉
        QCOMPARE(server.authorizationAt(0), QByteArray("Bearer test-key"));
        const QJsonObject body = server.requestObjectAt(0);
        QCOMPARE(body.value("stream").toBool(), true);
        QCOMPARE(body.value("model").toString(), QStringLiteral("test-model"));
    }

    void mergesToolCallFragmentsByIndex()
    {
        MockLlmServer server({Turn{.frames = {
            toolCallStartChunk(1, "call_b", "execute_blender_code", R"({"code":"bpy.ops.mesh.)"),
            toolCallStartChunk(0, "call_a", "execute_blender_code", R"({"code":"import bpy")"),
            toolArgsChunk(1, R"(primitive_cube_add()})"),
            toolArgsChunk(0, R"(;bpy.ops.object.select_all()})"),
            doneChunk()}, .splitEvery = 4}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("建个盒子"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.toolCalls.size(), 2);                  // 两个 index 各一次，不重复上报
        QVERIFY(rec.deltas.isEmpty());                      // tool-only 回合不产生文本增量
        const QJsonObject first = rec.toolCalls.at(0).toObject();
        const QJsonObject second = rec.toolCalls.at(1).toObject();
        QCOMPARE(first.value("id").toString(), QStringLiteral("call_a"));  // 按 index 升序还原
        QCOMPARE(second.value("id").toString(), QStringLiteral("call_b"));
        QCOMPARE(second.value("arguments").toObject().value("code").toString(),
                 QStringLiteral("bpy.ops.mesh.primitive_cube_add()"));     // 分片拼完整并解析
    }

    void completesWhenFinalFrameHasNoBlankLine()
    {
        MockLlmServer server({Turn{
            .frames = {R"(data: {"choices":[{"delta":{"content":"尾"}}]})"}, // 无结尾 \n\n
            .splitEvery = 4}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.deltas, QStringList{"尾"});            // 靠 finished 里补 "\n\n" 冲出来
        QVERIFY(rec.errors.isEmpty());
    }

    void reportsErrorOnceOnHttpError()
    {
        MockLlmServer server({Turn{.status = 401}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0);
        QVERIFY(rec.deltas.isEmpty());
        QCOMPARE(server.requestCount(), 1);                 // 不自动重试
    }

    void reportsErrorWhenConnectionRefused()
    {
        quint16 deadPort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            deadPort = probe.serverPort();
        }                                                   // 作用域结束即关闭，端口无人监听

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(LlmConfig{QStringLiteral("http://127.0.0.1:%1/v1").arg(deadPort),
                                     QStringLiteral("test-key"), QStringLiteral("test-model")},
                           userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0);                              // 连接失败绝不能走 onDone
    }

    void keepsConcurrentStreamsSeparate()
    {
        MockLlmServer alpha({Turn{.frames = {textChunk("alpha1"), textChunk("alpha2"), doneChunk()},
                                  .splitEvery = 5, .frameDelayMs = 15}});
        MockLlmServer beta({Turn{.frames = {textChunk("beta1"), textChunk("beta2"), doneChunk()},
                                 .splitEvery = 7, .frameDelayMs = 25}});
        QVERIFY(alpha.listen());
        QVERIFY(beta.listen());

        LlmGateway gateway;                                 // 同一个 gateway 上两条流同时在途
        Recorder a;
        Recorder b;
        gateway.chatStream(alpha.config(), userMessages("a"), {}, a.handlers());
        gateway.chatStream(beta.config(), userMessages("b"), {}, b.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(a.done + b.done == 2, 8000);
        QCOMPARE(a.deltas, (QStringList{"alpha1", "alpha2"}));
        QCOMPARE(b.deltas, (QStringList{"beta1", "beta2"}));
    }

    void abortEndsInFlightStreamsOnce()
    {
        MockLlmServer server({Turn{.frames = {textChunk("半句")}, .stallAfterFrames = true}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.deltas.size() == 1, 5000); // 首帧已到，连接仍开着
        gateway.abort();
        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0);                              // 中止不算正常完成
        gateway.abort();                                    // 重复 abort 不应再报错
        QTest::qWait(50);
        QCOMPARE(rec.errors.size(), 1);
    }
};

QTEST_MAIN(LlmGatewayTest)
#include "tst_llm_gateway.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --config Debug --target tst_llm_gateway`
Expected: 编译失败 `fatal error C1083: 无法打开包括文件: "mock/MockLlmServer.h"`（测试先于实现存在）。

- [ ] **Step 3: 实现 `MockLlmServer`（真实 HTTP 服务端）**

```cpp
// tests/mock/MockLlmServer.h
#pragma once

#include "core/llm/LlmGateway.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>

class QTcpSocket;

namespace MockLlm {
QByteArray textChunk(const QString& text);
QByteArray toolCallStartChunk(int index, const QString& id, const QString& name,
                              const QString& argsFragment);
QByteArray toolArgsChunk(int index, const QString& argsFragment);
QByteArray toolCallChunk(const QString& name, const QString& args = QStringLiteral("{}"));
QByteArray doneChunk();
} // namespace MockLlm

class MockLlmServer : public QObject
{
    Q_OBJECT
public:
    struct Turn
    {
        QList<QByteArray> frames;
        int splitEvery = 3;
        int frameDelayMs = 2;
        int status = 200;
        QByteArray errorBody = R"({"error":{"message":"mock error"}})";
        bool stallAfterFrames = false;
    };

    explicit MockLlmServer(QList<Turn> script, QObject* parent = nullptr);
    bool listen();
    QString baseUrl() const;
    LlmConfig config(const QString& model = QStringLiteral("test-model")) const;
    int requestCount() const { return int(m_requests.size()); }
    QString pathAt(int index) const { return m_paths.value(index); }
    QByteArray authorizationAt(int index) const { return m_authorizations.value(index); }
    QString requestAt(int index) const { return m_requests.value(index); }
    QJsonObject requestObjectAt(int index) const;

private:
    void onNewConnection();
    void handleRequest(QTcpSocket* socket, const QByteArray& body);
    void respond(QTcpSocket* socket, const Turn& turn);
    Turn takeTurn();

    QTcpServer m_server;
    QList<Turn> m_script;
    int m_next = 0;
    QList<QString> m_requests;          // compact JSON 文本
    QList<QString> m_paths;
    QList<QByteArray> m_authorizations;
};
```

```cpp
// tests/mock/MockLlmServer.cpp（关键部分；完整实现按此展开）
MockLlmServer::MockLlmServer(QList<Turn> script, QObject* parent)
    : QObject(parent)
    , m_script(std::move(script))
{
    connect(&m_server, &QTcpServer::newConnection, this, &MockLlmServer::onNewConnection);
}

QJsonObject MockLlmServer::requestObjectAt(int index) const
{
    return QJsonDocument::fromJson(requestAt(index).toUtf8()).object();
}

bool MockLlmServer::listen()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QString MockLlmServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort());
}

void MockLlmServer::onNewConnection()
{
    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;                                  // 头还没收齐
            const QByteArray head = buffer->left(headerEnd);
            int contentLength = 0;
            for (const QByteArray& line : head.split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    contentLength = line.mid(15).trimmed().toInt();
            }
            const QByteArray body = buffer->mid(headerEnd + 4);
            if (body.size() < contentLength)
                return;                                  // body 还没收齐，继续等
            socket->disconnect(socket, &QTcpSocket::readyRead, this, nullptr);

            m_paths << QString::fromUtf8(head.split(' ').value(1));
            m_authorizations << [&head]() {
                for (const QByteArray& line : head.split('\n'))
                    if (line.toLower().startsWith("authorization:"))
                        return line.mid(14).trimmed();
                return QByteArray{};
            }();
            m_requests << QString::fromUtf8(body.left(contentLength));
            respond(socket, takeTurn());
        });
    }
}
```

```cpp
void MockLlmServer::respond(QTcpSocket* socket, const Turn& turn)
{
    if (turn.status != 200) {                            // 错误分支：不发 SSE
        socket->write("HTTP/1.1 " + QByteArray::number(turn.status) + " Error\r\n"
                      "Content-Type: application/json\r\nConnection: close\r\n"
                      "Content-Length: " + QByteArray::number(turn.errorBody.size())
                          + "\r\n\r\n" + turn.errorBody);
        socket->disconnectFromHost();
        return;
    }

    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                  "Cache-Control: no-cache\r\nConnection: close\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n");

    // 先拼成一整条字节流再切片：切点自然会落在 "data:"、JSON、甚至 UTF-8 多字节中间
    auto slices = std::make_shared<QList<QByteArray>>();
    const int step = qMax(1, turn.splitEvery);
    for (const QByteArray& frame : turn.frames)
        for (int i = 0; i < frame.size(); i += step)
            slices->append(frame.mid(i, step));

    auto index = std::make_shared<int>(0);
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [socket, slices, index, pump, turn]() {
        if (*index >= slices->size()) {
            if (!turn.stallAfterFrames) {
                socket->write("0\r\n\r\n");               // chunked 结束
                socket->flush();
                socket->disconnectFromHost();
            }
            return;
        }
        const QByteArray slice = slices->at((*index)++);
        socket->write(QByteArray::number(slice.size(), 16) + "\r\n");
        socket->write(slice);
        socket->write("\r\n");
        socket->flush();                                  // 不 flush 会被 Qt 攒包，测不到分片
        QTimer::singleShot(turn.frameDelayMs, socket, [pump]() { (*pump)(); });
    };
    (*pump)();
}

MockLlmServer::Turn MockLlmServer::takeTurn()
{
    if (m_next < m_script.size())
        return m_script.at(m_next++);
    Turn exhausted;                                       // 脚本用超：500，避免测试挂死
    exhausted.status = 500;
    return exhausted;
}
```

`textChunk` / `toolCallStartChunk` / `toolArgsChunk` / `toolCallChunk` / `doneChunk` 统一构造 `choices[0].delta` 的 wire format 帧，末尾统一补 `\n\n`。

- [ ] **Step 4: CMake + 跑测试**

```cmake
add_executable(tst_llm_gateway tests/tst_llm_gateway.cpp tests/mock/MockLlmServer.cpp)
target_include_directories(tst_llm_gateway PRIVATE tests)
target_link_libraries(tst_llm_gateway PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_llm_gateway COMMAND tst_llm_gateway)
set_tests_properties(tst_llm_gateway PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
```

Run: `ctest --test-dir build -C Debug -R tst_llm_gateway --output-on-failure`
Expected: 7 用例 Passed。若用例 7（abort）红，改 `LlmGateway::abort` 的实现使其只上报一次，**不要放宽断言**。

- [ ] **Step 5: 去掉 `virtual`，编译期封死子类 mock**

`src/core/llm/LlmGateway.h` 中 `virtual void chatStream(...)` → `void chatStream(...)`，全量构建确认无子类（`rg "public LlmGateway"` 应无输出）。Task 2 / Task 3 的测试改为「真实 `LlmGateway` + `MockLlmServer`」（见 Task 3 片段）。

- [ ] **Step 6: Commit**（`test: LlmGateway 流式专项测试（真实 HTTP+SSE，禁止子类 mock）`）

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

- [ ] **Step 3: 单测以集成覆盖（Task 1B 的 `MockLlmServer` + 真实 `LlmGateway` 跑 HTTP+SSE；Task 3 的执行链用例顺带覆盖草稿流）；提交**（`feat: 总结 agent 草稿生成与持久化`）

---

### Task 2B: BlenderGate（Blender 会话互斥租约）

> **为何存在**：blender 区的低模 / 修改任务与 unity 区的 glb→fbx 转换复用同一个 Blender MCP 会话，而转换脚本会 `read_factory_settings(use_empty=True)` 清空场景，必须与 Blender agent 任务互斥。仅检查 `TaskManager::runningCount()` 存在 TOCTOU（检查后、真正调用前被另一区抢先），因此用一把显式的单 owner 租约。

**Files:** Create `src/core/mcp/BlenderGate.h/.cpp`、`tests/tst_blender_gate.cpp`；Modify `CoreApp.h`（值成员 `BlenderGate m_blenderGate;`）、`CMakeLists.txt`。

**Interfaces:**
- `class BlenderGate`（非 QObject，只在 CoreApp / UI 线程使用）：
  - `bool tryAcquire(const QString& owner)`：空闲则记录 owner 并返回 true；已占用返回 false（同 owner 也不可重入）。
  - `void release(const QString& owner)`：仅 owner 匹配时清空；不匹配静默忽略，防误释放他人租约。
  - `bool isBusy() const`；`QString owner() const`。
- owner 约定：`"blender-agent"`（低模 / 修改任务全程持有）、`"unity-convert"`（多附件转换全程持有）。
- Consumes: 无。Produces: `AgentOrchestrator::setBlenderGate()`（Task 3）、`plan.confirm` 预检查（Task 4）、`AssetImporter::setBlenderGate()`（P6 Task 4）。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QtTest>
#include "core/mcp/BlenderGate.h"

class BlenderGateTest : public QObject
{
    Q_OBJECT
private slots:
    void acquireAndRelease()
    {
        BlenderGate gate;
        QVERIFY(!gate.isBusy());
        QVERIFY(gate.tryAcquire(QStringLiteral("blender-agent")));
        QVERIFY(gate.isBusy());
        QCOMPARE(gate.owner(), QStringLiteral("blender-agent"));
        QVERIFY(!gate.tryAcquire(QStringLiteral("unity-convert"))); // 不可抢占
        gate.release(QStringLiteral("unity-convert"));               // 非 owner 释放无效
        QVERIFY(gate.isBusy());
        gate.release(QStringLiteral("blender-agent"));
        QVERIFY(!gate.isBusy());
        QVERIFY(gate.tryAcquire(QStringLiteral("unity-convert")));   // 释放后可再获取
    }
};

QTEST_MAIN(BlenderGateTest)
#include "tst_blender_gate.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `ctest --test-dir build -C Debug -R tst_blender_gate --output-on-failure`
Expected: FAIL（`BlenderGate` 未定义 / target 未加入 CMake）

- [ ] **Step 3: 实现**

```cpp
#pragma once

#include <QString>

class BlenderGate
{
public:
    bool tryAcquire(const QString& owner);
    void release(const QString& owner);
    bool isBusy() const { return !m_owner.isEmpty(); }
    QString owner() const { return m_owner; }

private:
    QString m_owner;
};
```

```cpp
#include "core/mcp/BlenderGate.h"

bool BlenderGate::tryAcquire(const QString& owner)
{
    if (!m_owner.isEmpty())
        return false;
    m_owner = owner;
    return true;
}

void BlenderGate::release(const QString& owner)
{
    if (m_owner == owner)
        m_owner.clear();
}
```

- [ ] **Step 4: CMake + 跑测试 + Commit**

`synthforge_core` 源列表追加 `src/core/mcp/BlenderGate.cpp`；追加 target：

```cmake
add_executable(tst_blender_gate tests/tst_blender_gate.cpp)
target_link_libraries(tst_blender_gate PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_blender_gate COMMAND tst_blender_gate)
```

Run: `ctest --test-dir build -C Debug -R tst_blender_gate --output-on-failure`
Expected: 1 用例 Passed。

```bash
git add CMakeLists.txt src/core/mcp/BlenderGate.h src/core/mcp/BlenderGate.cpp tests/tst_blender_gate.cpp
```

Commit（`feat: BlenderGate 会话互斥租约`）。

---

### Task 3: AgentOrchestrator（Planner + Executor 两阶段）

**Files:** Create `src/core/agent/AgentOrchestrator.h/.cpp`、`web/src/components/PlanCard.vue`、`tests/tst_orchestrator.cpp`；Modify `CoreApp`（draft.send → 启动规划；plan.confirm / plan.cancel 在 Task 4 接线）。测试里的 LLM 一律复用 Task 1B 的 `tests/mock/MockLlmServer.h/.cpp` + 真实 `LlmGateway`，不新建任何 `LlmGateway` 子类。

**Interfaces:**
- 计划结构：

```cpp
struct BuildPlacement { QString name; QString where; };
struct BuildGroup {
    QString title;
    QStringList items;
    QVector<BuildPlacement> placements;   // 每个 item 一条：相对谁、朝哪、离地多少
    QString acceptance;
};
struct BuildPlan {
    QVector<BuildGroup> groups;
    QStringList rules;                    // 本任务特有的跨组布局约束
    int maxSteps = 50;
};
```

布局信息分三层，不得混用：
1. **全局纪律**（`blenderSystemPrompt()` 固定文本）：贴地、不重叠、朝向基准、常用尺寸。每条任务都有，planner 不重复生成。
2. **任务约束**（`BuildPlan::rules`）：planner 从提示词里抽出的、跨组生效的特殊要求，如「树不遮挡招牌」「路灯沿人行道每 8 米一盏」。只写本任务特有，不写通用规则。
3. **物体摆放**（`BuildGroup::placements`）：逐物体描述相对关系与朝向，如「商店主体 → 底座组路面北侧边缘，正门朝 -Y」。**只允许相对描述，不允许绝对坐标**：LLM 算不准坐标，执行时由 agent 查已有物体的包围盒自行推算。

- `class AgentOrchestrator : QObject`
  - `void plan(const QString& taskId, TaskManager* tm, const QString& prompt, const QString& area);`：一次**纯文本**调用（不传 tools），解析 JSON（含 `rules` 与每组 `placements`）；失败重试 1 次，仍失败降级默认四分组（`rules` 与 `placements` 为空，交给全局纪律兜底）。
    - `void execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools, const BuildPlan& plan, const QString& prompt, const QString& targetFolder, const QString& area);`：按组「建造 → 自检」状态机执行。`tools` 必须是 **OpenAI tools 格式**，由 CoreApp 调用前用 `ToolBridge::mcpToolsToOpenAi(m_blenderTools)` 转换；编排层不感知 MCP `inputSchema`。
  - 信号：`planReady(area, taskId, QJsonObject plan)`、`groupProgress(area, int index, int count, QString title)`、`finalText(area, QString)`、`failed(area, QString)`、`toolStep(area, tool, status)`、`delta(area, text)`、`imageReceived(area, taskId, dataUrl, caption)`、`artifactsReady(area, QStringList absoluteGlbs)`（结束时扫描目标目录新 GLB，供 CoreApp 广播 `asset.created` / `fileTree.changed`）。
  - `void setBlenderGate(BlenderGate* gate)` — 仅注入给 blender orchestrator（unity orchestrator 不注入）。`execute()` 在 `area=="blender"` 时以 owner `"blender-agent"` 获取租约，持有到 `finishOk/finishCancelled/fail`；忙则直接 `failed("blender-busy")`，不碰 LLM / MCP。
  - `void setToolCallHook(std::function<void(const QString& name, bool running)> hook)` — P6 注入：工具调用前 running=true（进入抑制窗口），回调返回后 running=false（结束抑制）。
  - `void setConversationHistory(const QJsonArray& history)` — 规格 8.4 上下文组装：`system + 本区最近 20 条纯文本历史 + 本次任务指令`；CoreApp 在每次 `plan()/execute()` 前用 `m_sessions->recentMessages(area, 20)` 注入（成员 `QJsonArray m_history`）。
- 事件：`plan.created {area, taskId, rules, groups, totalSteps}`（每组含 `placements`）；`task.progress {area, groupIndex, groupCount, title}`；`tool.step` 沿用；结束时 `asset.created`（Task 4 编排）。
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
class BlenderGate;

struct BuildPlacement { QString name; QString where; };
struct BuildGroup { QString title; QStringList items; QVector<BuildPlacement> placements; QString acceptance; };
struct BuildPlan { QVector<BuildGroup> groups; QStringList rules; int maxSteps = 50; };

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
    void setBlenderGate(BlenderGate* gate) { m_gate = gate; }
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
    void releaseGate();
    QStringList collectNewGlbs() const;

    LlmGateway* m_llm;
    McpClient* m_mcp;
    TaskManager* m_tm = nullptr;
    LlmConfig m_cfg;
    QJsonArray m_tools;                // OpenAI tools（CoreApp 经 ToolBridge 转换后传入）
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
    BlenderGate* m_gate = nullptr;     // 仅 blender orchestrator 注入
    bool m_gateHeld = false;
    std::function<void(const QString&, bool)> m_toolCallHook;
};
```

> `setModelConfig` 由 CoreApp 在每次 `plan()/execute()` 前用主 agent 配置调用；`m_cfg` 缺省为空时 `LlmGateway` 会报 `agent-not-configured`（不会静默发出错误请求）。

- [ ] **Step 1: 规划阶段测试（红）**

```cpp
void plannerParsesGroupsAndLayout()
{
    MockLlmServer server({textChunk(R"({"rules":["树不遮挡招牌"],
        "groups":[
        {"title":"底座与环境","items":["底座","路牌"],
         "placements":[
            {"name":"底座","where":"场景中心，沿 X 轴展开"},
            {"name":"路牌","where":"商店右前方 2 米，正面朝 -Y"}],
         "acceptance":"俯视截图：底座完整"},
        {"title":"建筑外观","items":["墙体","招牌"],
         "placements":[
            {"name":"墙体","where":"底座北侧边缘"},
            {"name":"招牌","where":"正门上方，离地 2.8 米"}],
         "acceptance":"街角截图：主体齐全"}]})")});
    LlmGateway llm;                              // 真实网关，禁止子类重写 chatStream
    AgentOrchestrator orch(&llm, nullptr, nullptr);
    orch.setModelConfig(server.config());        // baseUrl 指向本地假服务端
    QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

    orch.plan("t1", nullptr, "做一个雨夜便利店", "blender");

    QTRY_COMPARE(ready.count(), 1);
    const QJsonObject plan = ready.first().at(2).toJsonObject();
    QCOMPARE(plan.value("groups").toArray().size(), 2);
    QCOMPARE(plan.value("totalSteps").toInt(), 26);   // 8*2+10
    QCOMPARE(plan.value("rules").toArray().size(), 1);
    const QJsonObject first = plan.value("groups").toArray().at(0).toObject();
    const QJsonArray placements = first.value("placements").toArray();
    QCOMPARE(placements.size(), 2);
    QCOMPARE(placements.at(1).toObject().value("name").toString(), QStringLiteral("路牌"));
    QVERIFY(placements.at(1).toObject().value("where").toString().contains("右前方"));
}

void plannerFallsBackToDefaults()
{
    MockLlmServer server({textChunk("抱歉，我无法输出 JSON。"), textChunk("仍然无法输出。")});
    LlmGateway llm;
    AgentOrchestrator orch(&llm, nullptr, nullptr);
    orch.setModelConfig(server.config());
    QSignalSpy ready(&orch, &AgentOrchestrator::planReady);

    orch.plan("t1", nullptr, "做一个雨夜便利店", "blender");

    QTRY_COMPARE(ready.count(), 1);
    const QJsonObject plan = ready.first().at(2).toJsonObject();
    QCOMPARE(plan.value("groups").toArray().size(), 4);   // 默认四分组
    QCOMPARE(plan.value("totalSteps").toInt(), 42);
    QCOMPARE(server.requestCount(), 2);                    // 重试 1 次
}
```

> 文件顶部 `using namespace MockLlm;`（`textChunk` / `toolCallChunk` 来自 Task 1B 的 `tests/mock/MockLlmServer.h`）。测试里的 `LlmGateway` 必须是真实对象，`MockLlmServer` 只当 HTTP 服务端，不得继承 `LlmGateway`。

- [ ] **Step 2: 规划实现（绿）**

```cpp
static const char* kPlanSystemPrompt =
    "你是低模场景建造的规划器。只输出一个 JSON 对象，不要任何解释或代码块围栏，格式："
    "{\"rules\":[\"本任务特有的跨组约束\"],\"groups\":[{\"title\":\"分组名\","
    "\"items\":[\"物体1\"],"
    "\"placements\":[{\"name\":\"物体1\",\"where\":\"相对位置与朝向\"}],"
    "\"acceptance\":\"自检时截图检查什么\"}]}。"
    "要求：3~6 组，覆盖用户提示词中全部必须独立的物体与数量下限；按施工顺序排列"
    "（底座环境 → 建筑外观 → 室内陈设 → 灯光特效）。"
    "items 里的每个物体必须在 placements 里有一条同名条目；where 只写相对关系不写绝对坐标"
    "（如「商店北侧边缘，正门朝 -Y」「招牌在正门上方，离地 2.8 米」）。"
    "rules 只写本任务特有、跨组生效的约束（如「树不遮挡招牌」「路灯每 8 米一盏」），"
    "不要重复贴地、不重叠、单位这类通用规则。";

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
    for (const QJsonValue& rule : obj.value("rules").toArray())
        plan.rules << rule.toString();
    for (const QJsonValue& value : obj.value("groups").toArray()) {
        const QJsonObject item = value.toObject();
        BuildGroup group;
        group.title = item.value("title").toString();
        for (const QJsonValue& name : item.value("items").toArray())
            group.items << name.toString();
        for (const QJsonValue& entry : item.value("placements").toArray()) {
            const QJsonObject entryObj = entry.toObject();
            BuildPlacement placement;
            placement.name = entryObj.value("name").toString();
            placement.where = entryObj.value("where").toString();
            if (!placement.name.isEmpty() && !placement.where.isEmpty())
                group.placements.push_back(placement);
        }
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
        {"底座环境", {}, {}, "俯视截图：底座完整、路面/标线齐全、无悬空凹陷"},
        {"建筑外观", {}, {}, "街角视角截图：主体、招牌、门窗齐全，比例协调"},
        {"店内陈设", {}, {}, "透过橱窗截图：按提示词店内清单逐项核对"},
        {"灯光特效", {}, {}, "夜景截图：招牌自发光、路灯、雨水粒子均已生效"},
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
            for (const BuildGroup& g : plan.groups) {
                QJsonArray placements;
                for (const BuildPlacement& p : g.placements)
                    placements.append(QJsonObject{{"name", p.name}, {"where", p.where}});
                groups.append(QJsonObject{{"title", g.title},
                                          {"items", QJsonArray::fromStringList(g.items)},
                                          {"placements", placements},
                                          {"acceptance", g.acceptance}});
            }
            m_planning.remove(m_area);
            emit planReady(m_area, m_taskId,
                           QJsonObject{{"taskId", m_taskId},
                                       {"rules", QJsonArray::fromStringList(plan.rules)},
                                       {"groups", groups},
                                       {"totalSteps", plan.maxSteps}});
        }});
}
```

- [ ] **Step 3: 执行状态机测试（红）**

```cpp
// 与 CoreApp 传入 execute 的格式一致：OpenAI tools，而不是 MCP 原始 tools
static QJsonArray openAiTools()
{
    return QJsonArray{
        QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{
                {"name", "execute_blender_code"},
                {"description", "执行 Blender Python 代码"},
                {"parameters", QJsonObject{
                    {"type", "object"},
                    {"properties", QJsonObject{
                        {"code", QJsonObject{{"type", "string"}}}
                    }}
                }}
            }}
        }
    };
}

void executorBuildThenSelfChecks()
{
    MockLlmServer server({toolCallChunk("execute_blender_code"),
                          textChunk("本组建造完成"),
                          textChunk("自检通过"),
                          textChunk("已导出 glb/fbx/blend")});
    LlmGateway llm;                              // 真实网关，脚本走真实 HTTP+SSE
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    orch.setModelConfig(server.config());
    BuildPlan plan;
    BuildGroup group;
    group.title = "底座与环境";
    group.items = {"底座"};
    group.placements = {{"底座", "场景中心，沿 X 轴展开"}};
    group.acceptance = "俯视截图检查底座完整";
    plan.groups = {group};
    plan.rules = {"树不遮挡招牌"};
    plan.maxSteps = 18;
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, openAiTools(), plan,
                 "做一个雨夜便利店", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QCOMPARE(server.requestCount(), 4);                  // 建造 → 自检 → 导出 → 收尾
    QVERIFY(server.requestAt(0).contains("沿 X 轴展开"));   // 组内摆放已注入建造指令
    QVERIFY(server.requestAt(2).contains("验收标准"));    // 自检指令已注入
    QVERIFY(server.requestAt(2).contains("树不遮挡招牌")); // 任务约束注入自检
    QVERIFY(server.requestAt(3).contains("export_scene")); // 导出指令已注入
}

void executorAdvancesGroups()
{
    MockLlmServer server({toolCallChunk("execute_blender_code"), textChunk("组1建造完成"),
                          textChunk("组1自检通过"),
                          toolCallChunk("execute_blender_code"), textChunk("组2建造完成"),
                          textChunk("组2自检通过"),
                          textChunk("已导出 glb/fbx/blend")});
    LlmGateway llm;
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    orch.setModelConfig(server.config());
    BuildPlan plan;
    plan.groups = {{"底座与环境", {}, {}, "检查底座"}, {"建筑外观", {}, {}, "检查建筑"}};
    QSignalSpy progress(&orch, &AgentOrchestrator::groupProgress);
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, openAiTools(), plan, "p", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QCOMPARE(progress.count(), 2);                      // 两组各一次进度
    QCOMPARE(progress.at(1).at(1).toInt(), 2);          // 第二组 index
}

// 文件顶部追加：#include "core/mcp/BlenderGate.h"
void executorRefusesWhenBlenderGateBusy()
{
    MockLlmServer server({});               // 不应发出任何请求
    LlmGateway llm;
    AgentOrchestrator orch(&llm, nullptr, nullptr);
    orch.setModelConfig(server.config());
    BlenderGate gate;
    QVERIFY(gate.tryAcquire(QStringLiteral("unity-convert")));
    orch.setBlenderGate(&gate);
    BuildPlan plan;
    plan.groups = {{"底座与环境", {}, {}, "检查底座"}};
    QSignalSpy failed(&orch, &AgentOrchestrator::failed);

    orch.execute("t1", nullptr, {}, plan, "p", "/tmp/out", "blender");

    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().at(1).toString(), QStringLiteral("blender-busy"));
    QCOMPARE(server.requestCount(), 0);                      // 门禁失败不碰 LLM / MCP
    QCOMPARE(gate.owner(), QStringLiteral("unity-convert")); // 不释放他人租约
}

void executorReleasesGateOnFinish()
{
    MockLlmServer server({toolCallChunk("execute_blender_code"),
                          textChunk("本组建造完成"), textChunk("自检通过"),
                          textChunk("已导出 glb/fbx/blend")});
    LlmGateway llm;
    MockMcpHarness mcp;
    AgentOrchestrator orch(&llm, &mcp.client(), nullptr);
    orch.setModelConfig(server.config());
    BlenderGate gate;
    orch.setBlenderGate(&gate);
    BuildPlan plan;
    plan.groups = {{"底座与环境", {}, {}, "检查底座"}};
    plan.maxSteps = 18;
    QSignalSpy finalSpy(&orch, &AgentOrchestrator::finalText);

    orch.execute("t1", nullptr, openAiTools(), plan, "p", "/tmp/out", "blender");

    QTRY_COMPARE(finalSpy.count(), 1);
    QVERIFY(!gate.isBusy());               // 成功出口必须释放
}
```

`MockLlmServer`（`tests/mock/MockLlmServer.h`，Task 1B 建立）：**不是 `LlmGateway` 的子类**，而是一个 `QTcpServer` 假服务端——解析请求行 / 头 / `Content-Length` body，回 `HTTP/1.1 200` + `Transfer-Encoding: chunked`，把脚本帧按 `splitEvery` 切片、按 `frameDelayMs` 逐片写出，让真实 `LlmGateway::chatStream` 走完整 HTTP + SSE 路径。`requestCount()` 与 `requestAt(i)`（compact JSON 文本）用于断言注入消息与请求体。`toolCallChunk/textChunk` 为脚本包装小工具；`toolCallChunk(name, args = "{}")` 必须生成 **OpenAI 流式 wire format** 的 SSE 分片（`choices[0].delta.tool_calls[{index,id,function:{name,arguments}}]`，arguments 为 JSON 字符串），确保测试覆盖真实的 function calling 解析路径，而不是用内部结构左手对右手。

- [ ] **Step 4: 执行状态机实现（绿）**

```cpp
static QString blenderSystemPrompt()
{
    return QStringLiteral("你是 SynthForge Studio 的 Blender 建模 agent。使用提供的 MCP 工具"
                          "（execute_blender_code 等）完成低模场景：先清场再分组建造，"
                          "必要时截图查看结果并修正；全部完成后按规范导出并落盘。\n"
                          "硬性约束（Unity 导入与跨工具复用依赖这三条，不得违反）：\n"
                          "1. 场景单位为米：1 Blender 单位 = 1 米（门高约 2.1，楼层约 3，人眼高 1.65）。\n"
                          "2. 每个道具的 origin 落在包围盒底部中心，摆到平面上不悬空。\n"
                          "3. 贴图必须打包进 .blend（导出前 pack_all），不得引用外部图片。\n"
                          "布局纪律（逐条执行，不得违反）：\n"
                          "4. 所有物体落地：object.location.z = 0，禁止悬空或埋地。\n"
                          "5. 建每个新物体前，先用 execute_blender_code 遍历 bpy.data.objects，"
                          "读出已有物体的 location 与 dimensions；新物体的水平投影不得与它们交叠。\n"
                          "6. 场景中心在原点，主建筑正面朝 -Y，人行道沿 X 轴展开。\n"
                          "7. 常用尺寸（米）：树 4~6，路灯 5，路牌 2.5，门 2.1，楼层 3，人 1.75。");
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
    if (m_gate && m_area == QLatin1String("blender")) {
        if (!m_gate->tryAcquire(QStringLiteral("blender-agent"))) {
            fail(QStringLiteral("blender-busy")); // 不排队：调用方提示用户稍后重试
            return;
        }
        m_gateHeld = true;
    }
    m_messages = initialMessages(
        blenderSystemPrompt(),
        QStringLiteral("已确认的专业提示词：\n%1\n目标存放目录：%2。").arg(prompt, targetFolder));

    // 清场：活会话里还留着上一次任务的模型，不清就会被 save_as_mainfile 一起存进新 blend
    // （先做超市再做路灯，路灯.blend 里就会带着超市）。确定性操作直调 MCP，不交给 LLM 记得。
    static const QString kClearScene = QStringLiteral(R"python(
import bpy
# 不用 read_factory_settings：那会重置 preferences，可能把 BlenderMCP 插件关掉、连接直接断
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images,
             bpy.data.armatures, bpy.data.actions, bpy.data.cameras, bpy.data.lights):
    for item in list(coll):
        if item.users == 0:
            coll.remove(item)
)python");
    if (!m_mcp) {                      // 单测/降级路径：没有 MCP 就没有会话可清
        startGroup();
        return;
    }
    m_mcp->callTool(QStringLiteral("execute_blender_code"),
                    QJsonObject{{"code", kClearScene}},
                    [this](const QJsonValue&, const QJsonObject& error) {
                        if (m_tm && m_tm->isCancelled(m_taskId)) {
                            finishCancelled();
                            return;
                        }
                        if (!error.isEmpty()) {
                            fail(QStringLiteral("clear-scene-failed"));
                            return;
                        }
                        startGroup();
                    });
}

void AgentOrchestrator::startGroup()
{
    const BuildGroup& group = m_plan.groups[m_groupIndex];
    emit groupProgress(m_area, m_groupIndex + 1, m_plan.groups.size(), group.title);
    QString content = QStringLiteral("开始第 %1/%2 组「%3」。本组对象：%4。")
                          .arg(m_groupIndex + 1)
                          .arg(m_plan.groups.size())
                          .arg(group.title,
                               group.items.isEmpty() ? QStringLiteral("按提示词清单执行")
                                                     : group.items.join(QStringLiteral("、")));
    if (!group.placements.isEmpty()) {
        QStringList lines;
        for (const BuildPlacement& p : group.placements)
            lines << QStringLiteral("- %1：%2").arg(p.name, p.where);
        content += QStringLiteral("\n摆放要求（相对关系优先，具体坐标自行根据已有物体的包围盒推算）：\n%1")
                       .arg(lines.join('\n'));
    }
    content += QStringLiteral("\n放置前先查询场景中已有物体的位置与尺寸，避免重叠。完成后停止调用工具。");
    m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
    m_stream.clear();
    tick();
}

void AgentOrchestrator::onTurnFinished()   // 一轮 LLM 无工具调用时进入
{
    if (m_phase == GroupPhase::Build) {
        m_phase = GroupPhase::SelfCheck;
        const BuildGroup& group = m_plan.groups[m_groupIndex];
        QString content = QStringLiteral("第 %1 组建造结束。请对照验收标准：%2。")
                              .arg(m_groupIndex + 1)
                              .arg(group.acceptance);
        if (!m_plan.rules.isEmpty())
            content += QStringLiteral("\n本任务布局约束（逐条核对，不满足则修正后再结束）：\n- %1")
                           .arg(m_plan.rules.join(QStringLiteral("\n- ")));
        content += QStringLiteral("\n调用截图工具检查本组结果，缺漏、悬空、穿过其他物体或明显偏差"
                                  "立即补建修正；确认无误后停止调用工具。");
        m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
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
                "全部组已完成。现在执行导出（execute_blender_code），把下面脚本原样执行，"
                "只把 <BASENAME> 换成草稿命名清单里的英文基名；基名撞车会自动加序号，不要改逻辑：\n"
                "import bpy, os\n"
                "folder = r\"%1\"\n"
                "name = \"<BASENAME>\"\n"
                "base, n = name, 1\n"
                "while os.path.exists(os.path.join(folder, base + \".blend\")):\n"
                "    n += 1\n"
                "    base = name + \"_\" + str(n)\n"
                "bpy.ops.export_scene.gltf(filepath=os.path.join(folder, base + \".glb\"),\n"
                "                          export_format='GLB', export_animations=True,\n"
                "                          export_animation_mode='ACTIONS', export_frame_range=True)\n"
                "bpy.ops.export_scene.fbx(filepath=os.path.join(folder, base + \".fbx\"))\n"
                "bpy.ops.file.pack_all()\n"
                "bpy.ops.wm.save_as_mainfile(filepath=os.path.join(folder, base + \".blend\"))\n"
                "print('EXPORTED_BASE=' + base)\n"
                "不要删除场景里已有的关键帧动画（导出时它们会作为 GLB 动画保留）。"
                "三个文件必须同基名；如果 base 和 name 不同（加了序号），在最终回复里告诉用户实际文件名；"
                "导出完成后停止调用工具。").arg(m_targetFolder)}});
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

void AgentOrchestrator::releaseGate()
{
    if (m_gateHeld && m_gate)
        m_gate->release(QStringLiteral("blender-agent"));
    m_gateHeld = false;
}

void AgentOrchestrator::finishOk(const QString& text)
{
    const QStringList glbs = collectNewGlbs();
    if (!glbs.isEmpty())
        emit artifactsReady(m_area, glbs); // 先报产出，再收尾对话
    emit finalText(m_area, text);
    releaseGate();                        // 先释放门禁，再推进同区队列
    if (m_tm)
        m_tm->finish(m_taskId);
    m_taskId.clear();
}

void AgentOrchestrator::finishCancelled()
{
    m_planning.remove(m_area);
    releaseGate();
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
    if (m_taskId.isEmpty()) {
        releaseGate();                    // 防御：无任务 id 也不遗留租约
        return;
    }
    const QString normalized = reason == QLatin1String("max steps exceeded")
                                   ? QStringLiteral("max-steps-exceeded")
                                   : reason;
    emit failed(m_area, normalized);
    releaseGate();
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

`executeToolCalls`（含工具调用钩子与截图回传；`AgentOrchestrator.cpp` 顶部 `#include "core/llm/ToolBridge.h"`）。

> **不接 `progressToken`**：工具执行期 UI 只用 `toolStep(area, tool, "running" / "done" / "error")` 转圈，不显示百分比。`McpClient::callTool` 的第 4 参保持可选（不传就不写 `params._meta`，服务端不会发 `notifications/progress`）；将来需要细分进度时，在编排层补 `QHash<QString, ToolContext>` 映射即可，传输层不用改。

```cpp
void AgentOrchestrator::executeToolCalls(const QJsonArray& calls)
{
    m_awaitingTools = true;
    m_pendingToolCalls = calls.size();
    // calls 是 LlmGateway 的内部格式 {id,name,arguments(object)}；回灌历史必须转回 OpenAI wire format
    m_messages.append(QJsonObject{{"role", "assistant"},
                                  {"content", m_stream},
                                  {"tool_calls", ToolBridge::internalCallsToWire(calls)}});
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QString name = call.value("name").toString();
        const QJsonObject args = call.value("arguments").toObject();
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
m_orch->setBlenderGate(&m_blenderGate);   // 只给 blender orchestrator；unity orchestrator 不注入
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
                                  {"rules", plan.value("rules")},
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

> 每次 `plan()/execute()` 之前调用 `m_orch->setModelConfig(primaryAgentConfig())` 与 `m_orch->setConversationHistory(m_sessions->recentMessages(area, 20))`；`m_orch` 加入 `CoreApp.h` 成员（`AgentOrchestrator* m_orch = nullptr;`），并 `#include "core/agent/AgentOrchestrator.h"`（`PendingPick` 持有 `BuildPlan` 需要完整类型）；`m_blenderGate` 是 Task 2B 的 CoreApp 值成员，`#include "core/mcp/BlenderGate.h"`。P6 Task 3 把同一组 connect 封装为 `wireOrchestrator(orch)`（含 `artifactsReady`），blender 与 unity 共用，P6 落地时删除本段内联 connect；`setBlenderGate` 不放进共用 wiring，只对 `m_orch` 调用一次。

- [ ] **Step 5: 前端 PlanCard.vue + ChatPanel 接入**

```vue
<script setup lang="ts">
import { ref } from "vue";
import type { WsClient } from "../ws";

const props = defineProps<{
  client: WsClient;
  area: string;
  plan: {
    taskId: string;
    rules: string[];
    groups: { title: string; items: string[]; placements: { name: string; where: string }[]; acceptance: string }[];
    totalSteps: number;
  };
}>();
const decided = ref<"pending" | "started" | "cancelled">("pending");
const error = ref("");

async function start() {
  error.value = "";
  const res: any = await props.client.request("plan.confirm", {
    area: props.area, taskId: props.plan.taskId,
  });
  if (res?.ok === false) {
    // blender-busy：计划卡保留，等当前建模/转换结束后可再次点开始
    error.value = res.error === "blender-busy"
      ? "Blender 会话正忙：请等当前建模/转换完成后再点开始"
      : (res.error ?? "确认失败");
    return;
  }
  decided.value = "started";
}
function cancel() {
  decided.value = "cancelled";
  props.client.request("plan.cancel", { area: props.area, taskId: props.plan.taskId });
}
</script>

<template>
  <div class="plan-card">
    <div class="plan-hint">建造计划 · 预估 {{ plan.totalSteps }} 步</div>
    <div class="plan-warn">开始后会先清空 Blender 当前场景（未保存的手工修改会丢失）</div>
    <div v-if="plan.rules?.length" class="plan-rules">
      <div class="rules-title">布局约束</div>
      <ul><li v-for="(r, i) in plan.rules" :key="i">{{ r }}</li></ul>
    </div>
    <ol>
      <li v-for="(g, i) in plan.groups" :key="i">
        <div class="group-title">{{ i + 1 }}. {{ g.title }}</div>
        <div v-if="g.items.length" class="group-items">{{ g.items.join(" / ") }}</div>
        <ul v-if="g.placements?.length" class="group-placements">
          <li v-for="(p, j) in g.placements" :key="j">{{ p.name }}：{{ p.where }}</li>
        </ul>
        <div class="group-accept">验收：{{ g.acceptance }}</div>
      </li>
    </ol>
    <div v-if="error" class="plan-error">{{ error }}</div>
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

样式里补 `.plan-warn { font-size: 12px; color: #d97706; margin: 2px 0 8px; }`、`.plan-rules { font-size: 12px; color: #475569; margin: 0 0 8px; }`、`.group-placements { margin: 2px 0 4px; padding-left: 18px; font-size: 12px; color: #64748b; list-style: disc; }`。

`ChatPanel.vue` 新增订阅 `plan.created` → 追加计划卡并 `busy.value = false`（等待用户决策）；计划卡 `start` 等待 `plan.confirm` 返回，`ok:false` 时保持 pending 并显示错误（`blender-busy` 时计划卡保留，可稍后再点）；`cancel` → `client.request("plan.cancel", {area, taskId})`；收到 `folderPick.open`（Task 4）后弹存放位置。

- [ ] **Step 6: CMake + 跑测试 + Commit**（`tst_orchestrator` 6 用例：规划解析（含布局与约束）/ 降级 / 单组执行（含摆放与约束注入）/ 多组推进 / 门禁忙拒绝 / 成功释放门禁；target 写成 `add_executable(tst_orchestrator tests/tst_orchestrator.cpp tests/mock/MockLlmServer.cpp)` + `target_include_directories(tst_orchestrator PRIVATE tests)`，脚本经真实 HTTP+SSE 由 `MockLlmServer` 喂入；`feat: Planner→Executor 两阶段主循环（计划卡/分组自检/布局约束）`）

---

### Task 4: folderPick 流程 + 资产落盘

**Files:** Create `src/core/assets/AssetLibrary.h/.cpp`、`tests/tst_asset_library.cpp`、`web/src/components/FolderPicker.vue`；Modify `CoreApp`（`folderPick.submit/cancel/mkdir`、`fs.operate`、`tree.refresh`、`asset.import` 预留）、`CMakeLists.txt`。

**Interfaces:**
- 事件 `folderPick.open {area, requestId, rootName:"ModelLibrary", tree}`（tree 由 `AssetLibrary::folderTree()` 生成，`{name, path, children}`）。
- 请求 `folderPick.submit {requestId, relativePath}` → 校验（必须已存在、必须在根内）→ 调用 `orchestrator.execute(taskId, tm, ToolBridge::mcpToolsToOpenAi(m_blenderTools), plan, prompt, targetFolder, area)`（plan/prompt 来自 `plan.confirm` 时缓存；`CoreApp.cpp` 顶部 `#include "core/llm/ToolBridge.h"`）。
- 门禁：`plan.confirm` 与 `folderPick.submit` 各做一次预检查（忙时分别保留计划卡 / 目录弹窗）；最终以 `execute()` 的 `tryAcquire("blender-agent")` 为准。若在提交与任务启动的极小窗口内被抢先，任务以 `blender-busy` 失败并提示稍后重试（不自动排队）。
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
    QStringList scanAssets() const;               // glb + fbx + blend，供文件树；AssetPicker 按 kind 过滤

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

QStringList AssetLibrary::scanAssets() const
{
    QStringList result;
    QDirIterator it(m_root, {"*.glb", "*.fbx", "*.blend"}, QDir::Files,
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
    if (!m_pendingPlans.contains(area))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "plan-not-found"}});
    if (m_blenderGate.isBusy()) {
        // 必须在 take 之前检查：忙时保留计划卡，用户等 Blender 空闲后可再次点开始
        m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", "blender-busy"}});
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "blender-busy"}});
    }
    const QJsonObject json = m_pendingPlans.take(area);
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
    const QString area = m_pickRequests.value(requestId);   // 先不 take：失败时弹窗可重试
    if (area.isEmpty() || !m_pendingPicks.contains(area))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "request-not-found"}});
    const QString relative = params.value("relativePath").toString();
    if (!m_library->isValidTarget(relative))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
    const PendingPick pick = m_pendingPicks.value(area);
    if (pick.mode != "cloud3d" && m_blenderGate.isBusy()) {
        // 保留 requestId 与 pendingPick：弹窗不关，Blender 空闲后可再次确认
        m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", "blender-busy"}});
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "blender-busy"}});
    }
    m_pickRequests.remove(requestId);
    m_pendingPicks.remove(area);
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
                 m_orch->execute(tid, tm,
                                 ToolBridge::mcpToolsToOpenAi(m_blenderTools), pick.plan,
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

> `folderPick.submit` 先检查门禁再消费 `requestId` / `pendingPick`：`blender-busy` 或 `invalid-path` 时弹窗保持打开，用户可修正路径或等 Blender 空闲后再次确认；成功后才 `remove`。随后把 TaskManager 回调参数 `tid` 传给 `execute()`（与入队 id 相同，但以回调参数为准）；此后由 `execute()` 内部 `finishOk/finishCancelled/fail` 调用 `TaskManager::finish` 推进队列。`plan.maxSteps` 直接采用 planReady 的 `totalSteps`（已在规划阶段算好并封顶 60）。

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

弹窗树（缩进列表 + 单选），顶部 `[新建文件夹]`（`window.prompt` 名称 → `folderPick.mkdir {requestId, path: selectedPath, name}`，用返回的 `tree` 刷新），底部 `[取消] [确认]`；确认发 `folderPick.submit {requestId, relativePath: 选中相对路径}` 并等待返回，`ok:false` 时不关闭弹窗并显示错误（`blender-busy` 提示等 Blender 空闲后重试，`invalid-path` 提示换目录）。props：`{client, area, requestId, tree}`（由 ChatPanel 在 `folderPick.open` 时传入；只渲染 `area` 匹配的弹窗）。样式复用设置弹窗的遮罩类。

- [ ] **Step 4: 手工验证**（真实 Blender：发送需求 → 草稿 → 发送 → 计划卡确认 → 选子文件夹（先新建一个）→ 模型按组生成并落盘 → `asset.created` 触发文件树刷新；右键重命名/删除后树刷新）

- [ ] **Step 5: Commit**（`feat: 存放位置确认框与 GLB 资产落盘；资产库文件操作（fs.operate）`）

---

### Task 5: 文件树数据源 + three.js GLB 预览

**Files:** Modify `web/src/components/FileTree.vue`、`PreviewPane.vue`、`App.vue`、`web/src/preview/file.ts`；Modify `CoreApp`（`assets.list`、`file.read`、`fileTree.changed`）。

**Interfaces:**
- 请求 `assets.list {}` → `[{name, relativePath, kind, size, modified}]`：`kind ∈ glb|fbx|blend`，**三种都返回**（目录由前端按相对路径聚合，FileTree 平铺三种文件；P6 AssetPicker 已在消费端按 `kind === "glb" || "fbx"` 过滤，不受影响）。
- 请求 `file.read {area, path}` → `{ok, kind:'text'|'image'|'model'|'info', content?, dataUrl?, language?, size?, modified?, blendPath?}`；`kind==='info'` 是模型文件的**文件信息卡**（fbx/blend 不读内容）；模型三种格式额外返回 `blendPath`（同目录同名 `.blend` 的相对路径，没有则空串），供修改入口判定。blender 区路径相对资产库根（复用 `isValidTarget`）；unity 区由 P6 接管（P4 返回 `area-not-supported`）。
- `PreviewPane` props 与 P3 Task 6 一致：`{area, file}`；`file.kind==='model'` 时用 three.js 加载 `file.dataUrl`；`file.kind==='info'` 渲染文件信息卡（类型/大小/修改时间/能否修改）；文件为 glb/fbx/blend 时**标题栏右侧显示 [修改此资产]**（emit `modify(path)`）；未选中 → 概览卡。加载完成后按包围球自动摆相机；`gltf.animations` 全部同时循环播放，底部显示播放/暂停与各段时长（无动画不显示），右上角常驻「完整材质、动画与特效请在 Blender 中查看」。
- `App.vue`：FileTree `select(path)` → `file.read` → `previewFile`。FileTree 右键「修改此资产」与 PreviewPane 的 `modify` 事件走同一个 `requestModify(path)`：先 `file.read` 取 `blendPath` → 有则设为 `modifyTarget`（传给 ChatPanel），无则弹窗（见 Task 6）。订阅 `asset.created` → 用其相对路径 `path` 自动加载新资产。
- **语义变更**：点文件只预览，不再等于修改；只有 [修改此资产] 按钮 / 右键菜单会把资产设为修改目标（替换 P4 早期的「选中即修改」）。

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
        QString mime = QStringLiteral("image/jpeg");
        if (suffix == QLatin1String("png"))
            mime = QStringLiteral("image/png");
        else if (suffix == QLatin1String("webp"))
            mime = QStringLiteral("image/webp");
        return QJsonValue(QJsonObject{
            {"ok", true}, {"kind", "image"},
            {"dataUrl", QStringLiteral("data:%1;base64,").arg(mime)
                            + QString::fromLatin1(file.readAll().toBase64())}});
    }
    if (suffix == "glb") {
        // 模型三种格式都回 blendPath：修改入口据此判断有没有可编辑源文件
        const QFileInfo blend(info.dir().filePath(info.completeBaseName()
                                                  + QStringLiteral(".blend")));
        return QJsonValue(QJsonObject{
            {"ok", true}, {"kind", "model"},
            {"dataUrl", QUrl::fromLocalFile(abs).toString()},
            {"blendPath", blend.exists()
                              ? m_library->relativeFor(blend.absoluteFilePath())
                              : QString()}});
    }
    // fbx / blend 不读内容（可能几十 MB），只回文件信息卡
    if (suffix == "fbx" || suffix == "blend") {
        const QFileInfo blend =
            suffix == "blend"
                ? info
                : QFileInfo(info.dir().filePath(info.completeBaseName()
                                                + QStringLiteral(".blend")));
        return QJsonValue(QJsonObject{
            {"ok", true}, {"kind", "info"},
            {"size", static_cast<double>(info.size())},
            {"modified", info.lastModified().toString(Qt::ISODate)},
            {"blendPath", blend.exists()
                              ? m_library->relativeFor(blend.absoluteFilePath())
                              : QString()}});
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

`assets.list` handler（文件树与 AssetPicker 的唯一数据源；`kind` 为不含点后缀，返回 glb/fbx/blend 三种；目录由前端按相对路径聚合）：

```cpp
m_router->registerHandler("assets.list", [this](const QJsonObject&) {
    QJsonArray items;
    for (const QString& relative : m_library->scanAssets()) {
        const QFileInfo info(m_library->absoluteFor(relative));
        items.append(QJsonObject{{"name", info.completeBaseName()},
                                 {"relativePath", relative},
                                 {"kind", info.suffix().toLower()},
                                 {"size", static_cast<double>(info.size())},
                                 {"modified", info.lastModified().toString(Qt::ISODate)}});
    }
    return QJsonValue(items);
});
```

> 若 WebEngine 拒绝 `file:///`（自定义 scheme 页面跨 local 资源），把 glb 分支改为 `data:application/octet-stream;base64,` + 文件字节（低模 GLB 通常 < 5MB，内存可接受）。

- [ ] **Step 1: 实现 PreviewPane 核心**

先扩 P3 的类型契约 `web/src/preview/file.ts`（新增 `model` 与三个可选字段）：

```ts
export type PreviewKind = "text" | "image" | "info" | "model";

export interface PreviewFile {
  path: string;
  kind: PreviewKind;
  content?: string;
  dataUrl?: string;
  language?: string;
  size?: number;       // info：字节数
  modified?: string;   // info：ISO 时间
  blendPath?: string;  // 模型文件同名 .blend 相对路径，无则空
}
```

`PreviewPane.vue`：

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
import type { PreviewFile } from "../preview/file";

hljs.registerLanguage("csharp", csharp);
hljs.registerLanguage("json", json);
hljs.registerLanguage("typescript", typescript);
hljs.registerLanguage("lua", lua);

const props = defineProps<{ area: "blender" | "unity"; file: PreviewFile | null }>();
const emit = defineEmits<{ modify: [path: string] }>();
const host = ref<HTMLDivElement>();
const isModel = computed(() => props.file?.kind === "model");
const loadError = ref("");
const clips = ref<{ name: string; duration: number }[]>([]); // glb 自带的动画片段
const playing = ref(true);

const ASSET_SUFFIXES = ["glb", "fbx", "blend"];
// 修改入口只属于 blender 模型库：P6 的 unity 树也会列 fbx，但不能走 .blend 修改流程
const isAssetFile = computed(
  () => props.area === "blender" && !!props.file
    && ASSET_SUFFIXES.includes(props.file.path.split(".").pop()?.toLowerCase() ?? ""),
);

function baseName(path: string) {
  return path.split("/").pop() ?? path;
}

function folderOf(path: string) {
  const parts = path.split("/");
  parts.pop();
  return parts.length ? parts.join("/") : "模型库根目录";
}

function typeLabel(path: string) {
  const suffix = path.split(".").pop()?.toLowerCase();
  return suffix === "glb" ? "GLB 模型（可直接预览）"
       : suffix === "fbx" ? "FBX 交换格式（导入 Unity 用）"
       : "Blender 源文件（修改用）";
}

function formattedSize(bytes?: number) {
  if (!bytes) return "—";
  return bytes >= 1024 * 1024
    ? `${(bytes / (1024 * 1024)).toFixed(1)} MB`
    : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

function formattedModified(iso?: string) {
  return iso ? new Date(iso).toLocaleString() : "—";
}

let renderer: THREE.WebGLRenderer;
let scene: THREE.Scene;
let camera: THREE.PerspectiveCamera;
let controls: OrbitControls;
let loaded: THREE.Group | null = null;
let mixer: THREE.AnimationMixer | null = null;
let frame = 0;
const clock = new THREE.Clock();

function resize() {
  if (!host.value) return;
  renderer.setSize(host.value.clientWidth, host.value.clientHeight);
  camera.aspect = host.value.clientWidth / host.value.clientHeight;
  camera.updateProjectionMatrix();
}

// 资产尺度差异很大（1m 到几十米）：按包围球重新摆相机，否则动画摆了也看不见
function fitCamera(object: THREE.Object3D) {
  const sphere = new THREE.Box3().setFromObject(object).getBoundingSphere(new THREE.Sphere());
  const size = Math.max(sphere.radius * 2, 0.1);
  camera.position.copy(sphere.center)
    .add(new THREE.Vector3(size * 0.55, size * 0.45, size * 0.7));
  camera.near = Math.max(size / 1000, 0.001);
  camera.far = size * 20;
  camera.updateProjectionMatrix();
  controls.target.copy(sphere.center);
  controls.update();
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
    const delta = Math.min(clock.getDelta(), 0.1); // 切标签页回来不跳帧
    if (mixer && playing.value)
      mixer.update(delta);
    controls.update();
    renderer.render(scene, camera);
  };
  loop();
});

watch(() => props.file, (f) => {
  loadError.value = "";
  playing.value = true;
  clips.value = [];
  if (loaded) {
    if (mixer) {
      mixer.stopAllAction();
      mixer.uncacheRoot(loaded);
      mixer = null;
    }
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
      fitCamera(loaded);
      // glTF 只能携带物体 TRS 关键帧：把自带动画全部同时循环播放
      if (gltf.animations.length) {
        mixer = new THREE.AnimationMixer(loaded);
        for (const clip of gltf.animations)
          mixer.clipAction(clip).setLoop(THREE.LoopRepeat, Infinity).play();
        clips.value = gltf.animations.map((c) => ({
          name: c.name || "动画",
          duration: c.duration,
        }));
      }
    },
    undefined,
    () => { loadError.value = "模型加载失败"; },
  );
});

onBeforeUnmount(() => {
  cancelAnimationFrame(frame);
  window.removeEventListener("resize", resize);
  if (mixer) {
    mixer.stopAllAction();
    mixer = null;
  }
  renderer.dispose();
});

function highlighted(content: string, language?: string) {
  const lang = language && hljs.getLanguage(language) ? language : "typescript";
  return hljs.highlight(content, { language: lang }).value;
}
</script>

<template>
  <section class="preview">
    <header class="panel-head">
      <span>预览</span>
      <span v-if="file" class="preview-path">{{ file.path }}</span>
      <button v-if="isAssetFile" class="modify-btn" @click="emit('modify', file!.path)">
        修改此资产
      </button>
    </header>

    <div class="preview-body">
      <div ref="host" class="canvas-host" :class="{ hidden: !isModel }" />
      <p v-if="isModel && loadError" class="info-card">{{ loadError }}</p>

      <!-- GLB 只承载几何、基础材质与物体关键帧；其余效果要回 Blender 看 -->
      <p v-if="isModel && !loadError" class="preview-note">
        完整材质、动画与特效请在 Blender 中查看
      </p>
      <div v-if="isModel && clips.length" class="anim-bar">
        <button class="anim-toggle" @click="playing = !playing">
          {{ playing ? "暂停" : "播放" }}
        </button>
        <span v-for="(clip, index) in clips" :key="index" class="anim-clip">
          {{ clip.name }} {{ clip.duration.toFixed(1) }}s
        </span>
      </div>

      <template v-if="!isModel && file">
        <img v-if="file.kind === 'image'" :src="file.dataUrl" class="preview-image" :alt="file.path" />
        <pre v-else-if="file.kind === 'text'" class="code"><code v-html="highlighted(file.content ?? '', file.language)" /></pre>
        <div v-else-if="file.kind === 'info'" class="info-card file-info">
          <h3>{{ baseName(file.path) }}</h3>
          <dl>
            <dt>类型</dt><dd>{{ typeLabel(file.path) }}</dd>
            <dt>大小</dt><dd>{{ formattedSize(file.size) }}</dd>
            <dt>修改时间</dt><dd>{{ formattedModified(file.modified) }}</dd>
            <dt>所在目录</dt><dd>{{ folderOf(file.path) }}</dd>
            <dt>可修改</dt>
            <dd>{{ file.blendPath ? "是 · 已找到同名的 .blend 源文件" : "否 · 没有 .blend 源文件" }}</dd>
          </dl>
          <p v-if="!file.blendPath" class="file-info-hint">
            3D 高模（云 API 生成）与外部导入的模型只能重新生成，无法对话修改。
          </p>
        </div>
        <div v-else class="info-card">
          <h3>{{ file.path }}</h3>
          <p>暂无可用预览</p>
        </div>
      </template>
      <div v-else-if="!isModel" class="info-card">
        <h3>{{ area === "blender" ? "资产库概览" : "工程信息" }}</h3>
        <p>{{ area === "blender" ? "点击 GLB 预览模型；FBX / BLEND 显示文件信息" : "选中文件后在此预览" }}</p>
      </div>
    </div>
  </section>
</template>

<style scoped>
.preview { flex: 1; min-width: 0; display: flex; flex-direction: column; background: #1e1e1e; }
.preview-path { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: #999; }
.modify-btn { flex: none; padding: 2px 10px; font-size: 12px; }
/* canvas 铺满内容区（不能 inset:0 到整个 section，否则盖住标题栏） */
.preview-body { position: relative; flex: 1; min-height: 0; display: flex; flex-direction: column; }
.canvas-host { position: absolute; inset: 0; }
.canvas-host.hidden { display: none; }
.preview-note {
  position: absolute; top: 10px; right: 12px; z-index: 1;
  margin: 0; padding: 4px 8px;
  font-size: 11px; color: #999;
  background: rgba(20, 20, 20, 0.65);
  border-radius: 3px;
  pointer-events: none; /* 不挡鼠标拖拽旋转 */
}
.anim-bar {
  position: absolute; left: 50%; bottom: 12px; transform: translateX(-50%); z-index: 1;
  display: flex; align-items: center; gap: 10px;
  max-width: calc(100% - 24px); overflow-x: auto;
  padding: 6px 12px;
  font-size: 12px; color: #ccc;
  background: rgba(20, 20, 20, 0.75);
  border-radius: 4px;
}
.anim-toggle { flex: none; padding: 2px 10px; font-size: 12px; }
.anim-clip { flex: none; color: #999; white-space: nowrap; }
.preview-image { max-width: 100%; max-height: 100%; margin: auto; object-fit: contain; }
.code { position: absolute; inset: 0; overflow: auto; margin: 0; padding: 12px; }
.info-card { margin: auto; text-align: center; color: #999; }
.file-info { min-width: 280px; text-align: left; }
.file-info h3 { margin: 0 0 12px; color: #ddd; font-size: 14px; }
.file-info dl { display: grid; grid-template-columns: auto 1fr; gap: 6px 16px; margin: 0; font-size: 12px; }
.file-info dt { color: #777; }
.file-info dd { margin: 0; word-break: break-all; }
.file-info-hint { margin: 12px 0 0; color: #d7a55a; font-size: 12px; }
</style>
```

依赖增量：`web/package.json` 加 `"three": "^0.169.0"`、`"@types/three": "^0.169.0"`（`highlight.js` 已在 P3 Task 6 安装）。

`FileTree.vue`：在 P3 Task 6 的右键菜单骨架上填数据：`assets.list` 拉取 → 目录按相对路径聚合成可折叠节点，**三种模型文件平铺列出**（每行 = 文件名 + 后缀标签；glb 正常色，fbx/blend 用 `--muted` 淡一档，让人一眼看出哪个能直接预览）；点击任意文件 emit `select(relativePath)`；`fileTree.changed` → 重新拉取；右键节点的 `path` 使用相对路径。

模型文件项的右键菜单在既有五项后追加一条 **修改此资产**（`emit("modify", path)`，由 App 统一判定）；目录项与根不显示该条。`.blend` / `.fbx` 同样提供入口——是否有可编辑源文件由 App 拿 `file.read` 的 `blendPath` 判断，不在树里做二次推断（避免树与后端各算一套）。

`App.vue` 接线（在 P3 Task 6 的 `previewFile` 基础上，新增 `modifyTarget`）：

```ts
// previewFile 已在 P3 Task 6 声明，直接复用
const modifyTarget = ref<string | null>(null); // 已归一化到 .blend 的修改目标

async function readFile(path: string) {
  return (await client.request("file.read", { area: active.value, path })) as any;
}

async function openPreview(path: string) {
  const res = await readFile(path);
  if (res.ok)
    previewFile.value = {
      path, kind: res.kind, content: res.content, dataUrl: res.dataUrl,
      language: res.language, size: res.size, modified: res.modified,
      blendPath: res.blendPath,
    };
}

// 修改入口：预览区按钮与树右键菜单共用。没有 .blend 时明确告知只能重新生成
async function requestModify(path: string) {
  const res = await readFile(path);
  if (!res.ok)
    return;
  if (!res.blendPath) {
    window.alert(
      "无法修改此资产\n\n" +
      "未找到同名的 .blend 源文件。\n" +
      "3D 高模（云 API 生成）与外部导入的模型不支持对话修改，只能重新生成。",
    );
    return;
  }
  modifyTarget.value = res.blendPath; // 输入框上方出现 chip，随下一条消息发给 chat.send
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

`<FileTree @select="openPreview" @modify="requestModify" ... />`、`<PreviewPane ... @modify="requestModify" />`、`<ChatPanel ... :selected-asset="modifyTarget" @clear-asset="modifyTarget = null" />`（Task 6）。

- [ ] **Step 2: 手工验收（P4 总验收）**

1. 真实链路：Blender 打开且插件运行；发送「做三个低模木箱」→ 草稿卡出现； 双击可编辑；发送 → 计划卡出现（分组/验收标准可读）→ [开始生成] → 选 `Props/` 子 目录 → 步骤卡逐步更新 + `task.progress` 显示当前组 → 对话区出现截图 → 完成后文件树出现 `Props/xxx.glb`，点击可预览旋转。
2. 接着再发一个不相关的任务（如「做一盏路灯」）：打开新生成的 `.blend`，场景里**只有路灯**，没有上一个任务的木箱。
3. 三文件平铺：同一目录下 `xxx.glb` / `xxx.fbx` / `xxx.blend` 三行都在；点 glb → 3D 预览；点 fbx 或 blend → 中间显示文件信息卡（类型/大小/修改时间/目录/可修改=是）。
4. 修改入口：点 glb → 预览区右上出现 [修改此资产] → 点击 → 输入框上方 chip 显示 `目标资产：xxx`（指向 .blend）；删掉该资产的 .blend 后再点 → 弹窗「无法修改此资产 …只能重新生成」。
5. 动画：生成一个带可导出动效的资产（门开合 / 招牌轻摆 / 指针转动）→ 预览自动循环播放，底部动画条列出各段名称与时长，暂停 / 播放按钮生效；无动画的资产不显示动画条；相机自动对准模型。
6. 拒绝路径：关掉 Blender 插件 → 发送 → 立刻出现 `blender-not-ready` 错误卡，无总结 agent 请求。（**Task 7 后改为**：仍先出草稿卡，点执行时才报。）

- [ ] **Step 3: Commit**（`feat: 资产文件树与 three.js GLB 预览`）

---

### Task 6: 修改已有资产（跳过草稿/计划）

**Files:** Modify `CoreApp`（`chat.send` 增加 `mode:"modify"` + `asset` 参数）、`web/src/components/ChatPanel.vue`（目标资产 chip）、`App.vue`（`modifyTarget` 传给 ChatPanel）；无新 C++ 模块（复用 `AgentOrchestrator::execute`）。

**Interfaces:**
- 前端：**不再「选中即修改」**。点文件只预览；[修改此资产] 按钮或右键菜单 → Task 5 的 `requestModify(path)` → `file.read` 取 `blendPath`：
  - 有 → `modifyTarget = blendPath`，ChatPanel 输入框上方显示 `目标资产：<name> ✕` chip；
  - 无 → 弹窗「无法修改此资产 / 未找到同名的 .blend 源文件 / 3D 高模（云 API 生成）与外部导入的模型不支持对话修改，只能重新生成」。
  - 发送 `chat.send {area:"blender", mode:"modify", asset:"道具/sword_01/sword_01.blend", text:"把剑改短一点"}`（`asset` 恒为 `.blend` 相对路径）。
- `CoreApp`：`mode=="modify"` → `checkLowpoly()` → **把 `asset` 归一化到同目录同名 `.blend`**（传 glb/fbx 也能工作，作为前端预检的兜底）→ 不存在/越界返回 `{"accepted":false, "error":"no-blend-source"}`（不占 error.card 字典）→ 构造单组计划并调 `execute()`，**不经过** SummarizerAgent、plan 卡、folderPick。
- 门禁：`execute()` 以 `"blender-agent"` 获取同一把 `BlenderGate`；Unity 转换进行中直接 `failed("blender-busy")`，用户等待后重新发送修改指令。
- prompt 包装：`"打开 .blend 源文件 <绝对路径>。要求：<用户原话>。保留场景里已有的可导出动效（物体关键帧），不要删除或覆盖。完成后覆盖导出同名 GLB/FBX 并保存 .blend。"`；`targetFolder = 资产所在目录`。
- 完成后 `asset.created` 同名覆盖并触发 `fileTree.changed`。
- 云 API 资产没有 `.blend`，天然落进「无源文件」分支；混合素材（外部导入 glb）同理。v1 不做版本管理。

- [ ] **Step 1: CoreApp 接线**

在 `chat.send` 的 Task 1 前置检查之后、lowpoly 总结 agent 分支之前插入：

```cpp
if (mode == "modify") {
    const QString area = params.value("area").toString(QStringLiteral("blender"));
    const QString asset = params.value("asset").toString();
    const QString text = params.value("text").toString().trimmed();
    // 修改锚点统一是 .blend：传 glb/fbx 时自动找同目录同名源文件
    QFileInfo info;
    if (!asset.isEmpty()) {
        info = QFileInfo(m_library->absoluteFor(asset));
        if (info.suffix().toLower() != QLatin1String("blend"))
            info = QFileInfo(info.dir().filePath(info.completeBaseName()
                                                 + QStringLiteral(".blend")));
    }
    const QString blendRelative =
        info.exists() ? m_library->relativeFor(info.absoluteFilePath()) : QString();
    if (asset.isEmpty() || text.isEmpty() || blendRelative.isEmpty()
        || !m_library->isValidTarget(blendRelative))
        return QJsonValue(QJsonObject{{"accepted", false}, {"error", "no-blend-source"}});
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
    const QString prompt = QStringLiteral("打开 .blend 源文件 %1。要求：%2。"
                                          "保留场景里已有的可导出动效（物体关键帧），不要删除或覆盖。"
                                          "完成后覆盖导出同名 GLB/FBX 并保存 .blend。")
                               .arg(info.absoluteFilePath(), text);
    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);
    const QString targetFolder = info.absolutePath();
    m_tasks->enqueue(
        Task{taskId, area, QString(),
             [this, plan, prompt, targetFolder, area](const QString& tid, TaskManager* tm) {
                 m_orch->setModelConfig(primaryAgentConfig());
                 m_orch->setConversationHistory(m_sessions->recentMessages(area, 20));
                 m_orch->execute(tid, tm, ToolBridge::mcpToolsToOpenAi(m_blenderTools),
                                 plan, prompt, targetFolder, area);
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
const emit = defineEmits<{ clearAsset: [] }>();

async function send() {
  const text = input.value.trim();
  if (!text) return;
  input.value = "";
  // modifyTarget 只由 [修改此资产] / 右键菜单设定，点文件不会触发修改
  const modify = props.area === "blender" && !!props.selectedAsset;
  const res: any = await props.client.request("chat.send", {
    area: props.area,
    // 有修改目标 → modify；否则 blender 区用顶部模式切换（lowpoly/cloud3d）
    mode: modify ? "modify" : props.area === "blender" ? mode.value : undefined,
    text,
    asset: modify ? props.selectedAsset : undefined,
  });
  if (res?.error === "no-blend-source") {
    window.alert("无法修改此资产：源文件已不存在，请重新选择。");
    emit("clearAsset");
    return;
  }
  busy.value = res?.accepted !== false; // 被前置检查拒绝时不进入“进行中”
  if (res?.accepted !== false && modify) emit("clearAsset"); // 一次修改一条指令
}
```

```vue
<div v-if="selectedAsset" class="asset-chip">
  目标资产：{{ selectedAsset.split("/").pop()?.replace(/\.blend$/, "") }}
  <button @click="emit('clearAsset')">✕</button>
</div>
```

`App.vue` 把 `modifyTarget`（已归一化的 `.blend` 相对路径）传给 `ChatPanel:selected-asset`；chip 的 `✕` 和提交成功后都清空。

- [ ] **Step 3: 手工验证**（点 glb 仅预览、**不应**出现 chip → 点 [修改此资产] → chip 出现且指向 .blend → 输入修改指令 → 步骤卡执行 → 检查 .glb 修改时间与预览更新 → 文件树刷新；确认全程未出现草稿卡/计划卡/位置弹窗；再对一个没有 .blend 的 glb 点按钮 → 弹窗提示只能重新生成）

- [ ] **Step 4: Commit**（`feat: 修改已有资产（跳过草稿与计划，直接执行）`）

---

### Task 7: 讨论模式 + 统一草稿卡（P4 收尾）

> **依赖**：Task 3（orchestrator）、Task 5（资产树/预览）、Task 6（modify 归一化）全部完成后才能开始。
>
> **本任务对前序任务的修订**：
> 1. `chat.send` 的 `lowpoly` / `modify` 不再入口预检，改到 `draft.send`；`cloud3d` 仍早拦。
> 2. 修改已有资产从「跳过所有确认直接干」改为「先出一张修改草稿卡」，`startModify` 从 Task 6 的 `chat.send` 分支抽成独立函数。
> 3. `drafts` 表与 `saveDraft` / `draft()` 删除，草稿改存 `messages.meta`。
> 4. Task 6 验收里「关掉 Blender 再发送 → 立刻报 `blender-not-ready`」被推翻：现在会先出草稿卡，点执行才报。
>
> **为何存在**：用户需要在动手前先和模型讨论方案（「这个招牌怎么改好看」「为什么预览里没有雨」），而现有每条消息都会直接进入建模链。本任务加一个「讨论 / 执行」开关：讨论模式只对话、不碰 Blender；讨论收敛后模型在回复末尾附一段可直接执行的草稿，前端把它转成草稿卡。同时把「任何执行前都有一张草稿卡」固化成统一规则：执行模式改已有资产也先出草稿卡。

**Files:**
- Create: `src/core/agent/AskReply.h/.cpp`、`src/resources/prompts/ask.md`、`src/resources/prompts/summarizer-modify.md`、`tests/tst_ask_reply.cpp`
- Modify: `src/core/store/SessionStore.h/.cpp`（messages 加 meta 列、删 drafts 表）、`src/core/agent/SummarizerAgent.h/.cpp`、`src/core/coreapp/CoreApp.h/.cpp`、`tests/tst_session_store.cpp`、`tests/tst_summarizer.cpp`、`CMakeLists.txt`、`web/src/components/ChatPanel.vue`、`web/src/components/DraftCard.vue`、`web/src/chat/history.ts`

**Interfaces:**
- `chat.send { area, mode: "ask", text, asset? }` → 流式 `chat.delta` → 收尾 `chat.message { role:"assistant", content }`；若回答里带了可执行块，紧跟一条 `draft.created { id, prompt, origin:"discussion", asset }`。ask 分支**不做 preflight**（不碰 Blender）、不占 `BlenderGate`、不进计划/执行链；`asset` 是当前选中资产的相对路径，可为空。
- 回答协议：模型在末尾输出 `[[EXEC]]…[[/EXEC]]` 块（ASCII 方括号，避免特殊字符在源码与模型输出里被改写）。后端在流式阶段把块剥掉，转成一条独立草稿消息；无块时只落 assistant 正文。
- `draft.created { id, prompt, origin, asset, mode }`：`id` 是 messages 表的行号，后续 `draft.save / send / regenerate / cancel` 全用它定位；`origin` 为 `discussion`（模型顺手写）或 `generated`（SummarizerAgent）。
- `draft.save { id, content }`：用户编辑草稿后失焦即存，刷新不丢。
- `draft.send { id, prompt }`：先把这一行改成 `role='user'`（不新增 user 消息，否则历史里重复两条），再按 meta.asset 分流——有资产走 `modify`，没有走 `plan()`。
- `draft.cancel { id }`：meta.state 改为 `cancelled`，卡片变灰只读。
- `draft.regenerate { id }`：从 meta 读 `asset` / `source`，从 content 读讨论产出，重新调 SummarizerAgent，结果 `updateMessageContent` 后广播 `draft.updated`。
- 前置检查位置：`chat.send` 的 `lowpoly` / `modify` **不再**在入口预检；统一在 `draft.send` 调 `checkLowpoly()`，失败弹 `error.card` 且草稿卡保留。理由：写方案只需要 LLM，Blender 没开也应能先把要求写好。
- `SummarizerAgent::generate(SummarizeRequest)`：

```cpp
struct SummarizeRequest {
    QString area;
    QString userText;
    QString mode = QStringLiteral("lowpoly"); // lowpoly | cloud3d | modify
    QString asset;      // modify：目标资产名（写进模板，避免改错对象）
    QString discussion; // 可选：讨论结论，追加在 system 末尾
};
```

- 前端开关：`ChatPanel` 输入区左侧 segmented control，默认「执行」；选「讨论」时发 `mode:"ask"`，选「执行」时有 `modifyTarget` 发 `mode:"modify"` + `asset`，否则发 `mode:"lowpoly"`。

- [ ] **Step 1: messages 表加 meta（红 → 绿）**

草稿卡刷新后能不能恢复，全靠这一列：`role='draft'` 的消息把 `asset` / `state` / `origin` / `source` 存在 `meta` 里，`id` 就是渲染顺序。

`tests/tst_session_store.cpp` 追加：

```cpp
    void draftRoundTripsWithMeta()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());

        const qint64 id = store.appendMessage(
            QStringLiteral("blender"), QStringLiteral("draft"),
            QStringLiteral("主题与视角：雨夜便利店"),
            QJsonObject{{QStringLiteral("state"), QStringLiteral("pending")},
                        {QStringLiteral("asset"), QStringLiteral("shop.glb")}});
        QVERIFY(id > 0);

        const MessagePage page = store.messagePage(QStringLiteral("blender"), 10);
        QCOMPARE(page.messages.size(), 1);
        const QJsonObject row = page.messages.first().toObject();
        QCOMPARE(row.value(QStringLiteral("id")).toInteger(), id);
        QCOMPARE(row.value(QStringLiteral("role")).toString(), QStringLiteral("draft"));
        QCOMPARE(row.value(QStringLiteral("meta")).toObject()
                     .value(QStringLiteral("asset")).toString(),
                 QStringLiteral("shop.glb"));
    }

    void confirmedDraftBecomesUserMessage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        const qint64 id = store.appendMessage(
            QStringLiteral("blender"), QStringLiteral("draft"), QStringLiteral("旧文本"),
            QJsonObject{{QStringLiteral("state"), QStringLiteral("pending")}});

        QVERIFY(store.updateMessageContent(id, QStringLiteral("改过的文本")));
        QVERIFY(store.setMessageRole(id, QStringLiteral("user")));

        const QJsonObject row = store.messageById(id);
        QCOMPARE(row.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        QCOMPARE(row.value(QStringLiteral("content")).toString(), QStringLiteral("改过的文本"));
    }
```

先跑一次，预期编译失败（`appendMessage` 还不接受第 4 个参数）。

`SessionStore.h`：

```cpp
    qint64 appendMessage(const QString& area, const QString& role, const QString& content,
                         const QJsonObject& meta = QJsonObject{});
    QJsonObject messageById(qint64 id) const;
    bool updateMessageContent(qint64 id, const QString& content);
    bool updateMessageMeta(qint64 id, const QJsonObject& meta);
    bool setMessageRole(qint64 id, const QString& role);
    // 删除 saveDraft / draft()：草稿卡改为 messages 里的一行，不再用独立表
```

同步删掉 `tests/tst_session_store.cpp` 里的 `draftUpsertKeepsLatest` 用例（它测的是即将消失的单值草稿表）。

`SessionStore.cpp` 四处：

```cpp
// open()：新库建表带 meta，旧库补列，顺手丢掉废弃的 drafts 表
q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS messages("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, area TEXT NOT NULL,"
                      "role TEXT NOT NULL, content TEXT NOT NULL, ts INTEGER NOT NULL,"
                      "meta TEXT NOT NULL DEFAULT '{}')"));
{
    // SQLite 的 ADD COLUMN 没有 IF NOT EXISTS：先查 PRAGMA 再补
    QSqlQuery probe(m_db);
    probe.exec(QStringLiteral("PRAGMA table_info(messages)"));
    bool hasMeta = false;
    while (probe.next()) {
        if (probe.value(1).toString() == QLatin1String("meta"))
            hasMeta = true;
    }
    if (!hasMeta)
        q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN meta TEXT NOT NULL DEFAULT '{}'"));
}
q.exec(QStringLiteral("DROP TABLE IF EXISTS drafts"));

// 写入返回自增 id
qint64 SessionStore::appendMessage(const QString& area, const QString& role,
                                   const QString& content, const QJsonObject& meta)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages(area, role, content, ts, meta) VALUES(?,?,?,?,?)"));
    q.addBindValue(area);
    q.addBindValue(role);
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
    if (!q.exec())
        return 0;
    return q.lastInsertId().toLongLong();
}

// 三种更新：内容（用户编辑 / 重新生成）、meta（取消失效）、角色（草稿转正式指令）
bool SessionStore::updateMessageContent(qint64 id, const QString& content)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET content=? WHERE id=?"));
    q.addBindValue(content);
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

bool SessionStore::updateMessageMeta(qint64 id, const QJsonObject& meta)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET meta=? WHERE id=?"));
    q.addBindValue(QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

bool SessionStore::setMessageRole(qint64 id, const QString& role)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET role=? WHERE id=?"));
    q.addBindValue(role);
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

// 单条读取：draft.send / draft.regenerate 要拿 meta 里的 asset / source
QJsonObject SessionStore::messageById(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, area, role, content, ts, meta FROM messages WHERE id=?"));
    q.addBindValue(id);
    q.exec();
    if (!q.next())
        return {};
    return QJsonObject{
        {QStringLiteral("id"), q.value(0).toLongLong()},
        {QStringLiteral("area"), q.value(1).toString()},
        {QStringLiteral("role"), q.value(2).toString()},
        {QStringLiteral("content"), q.value(3).toString()},
        {QStringLiteral("ts"), q.value(4).toLongLong()},
        {QStringLiteral("meta"),
         QJsonDocument::fromJson(q.value(5).toString().toUtf8()).object()}};
}
```

`messagePage` 与 `messages` 的 SELECT 都要带上 `meta`，并把列尾拼成对象：

```cpp
    q.prepare(QStringLiteral("SELECT id, role, content, ts, meta FROM messages "
                             "WHERE area=? ORDER BY id DESC LIMIT ?"));
    // ...
    newestFirst.append(QJsonObject{
        {QStringLiteral("id"), q.value(0).toLongLong()},
        {QStringLiteral("role"), q.value(1).toString()},
        {QStringLiteral("content"), q.value(2).toString()},
        {QStringLiteral("ts"), q.value(3).toLongLong()},
        {QStringLiteral("meta"),
         QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object()}});
```

Run: `ctest --test-dir build -C Debug -R tst_session_store --output-on-failure`
Expected: 原有用例 + 新增 2 用例全绿。

- [ ] **Step 2: 标记解析纯函数（红 → 绿）**

`tests/tst_ask_reply.cpp`：

```cpp
#include <QtTest>

#include "core/agent/AskReply.h"

class AskReplyTest : public QObject
{
    Q_OBJECT
private slots:
    void plainAnswerHasNoExecutable()
    {
        const AskReply r = parseAskReply(QStringLiteral("建议把招牌换成旧铁皮。"));
        QCOMPARE(r.visible, QStringLiteral("建议把招牌换成旧铁皮。"));
        QVERIFY(r.executable.isEmpty());
    }

    void execBlockIsSplitOut()
    {
        const AskReply r = parseAskReply(QStringLiteral(
            "建议从材质和破损两处入手。\n\n[[EXEC]]\n把招牌换成深绿做旧铁皮\n[[/EXEC]]"));
        QCOMPARE(r.visible, QStringLiteral("建议从材质和破损两处入手。"));
        QCOMPARE(r.executable, QStringLiteral("把招牌换成深绿做旧铁皮"));
    }

    void unclosedBlockTakesRest()
    {
        const AskReply r = parseAskReply(QStringLiteral("方案如下。\n[[EXEC]]\n主题与视角：雨夜街角"));
        QCOMPARE(r.visible, QStringLiteral("方案如下。"));
        QCOMPARE(r.executable, QStringLiteral("主题与视角：雨夜街角"));
    }

    void emptyBlockIsNotExecutable()
    {
        const AskReply r = parseAskReply(QStringLiteral("先不定。\n[[EXEC]][[/EXEC]]"));
        QCOMPARE(r.visible, QStringLiteral("先不定。"));
        QVERIFY(r.executable.isEmpty());
    }
};

QTEST_MAIN(AskReplyTest)
#include "tst_ask_reply.moc"
```

`src/core/agent/AskReply.h`：

```cpp
#pragma once

#include <QString>

// 讨论模式的回答分两部分：正文给用户看，[[EXEC]] 块是可直接执行的草稿。
struct AskReply
{
    QString visible;
    QString executable;
};

AskReply parseAskReply(const QString& raw);
```

`src/core/agent/AskReply.cpp`：

```cpp
#include "core/agent/AskReply.h"

namespace {
const QString kOpen = QStringLiteral("[[EXEC]]");
const QString kClose = QStringLiteral("[[/EXEC]]");
}

AskReply parseAskReply(const QString& raw)
{
    AskReply reply;
    const int start = raw.indexOf(kOpen);
    if (start < 0) {
        reply.visible = raw.trimmed();
        return reply;
    }
    const int bodyStart = start + kOpen.size();
    const int end = raw.indexOf(kClose, bodyStart);
    // 模型偶尔忘写闭合标记：把剩余内容整段当成草稿，不让用户看到半截标记
    reply.executable = (end < 0 ? raw.mid(bodyStart) : raw.mid(bodyStart, end - bodyStart)).trimmed();
    reply.visible = raw.left(start).trimmed();
    return reply;
}
```

Run: `ctest --test-dir build -C Debug -R tst_ask_reply --output-on-failure`
Expected: 4 用例 Passed。

- [ ] **Step 3: SummarizerAgent 多模板（红 → 绿）**

`tests/tst_summarizer.cpp` 追加两个用例（同时把现有 4 处 `agent.generate(X, Y)` 改成 `agent.generate({X, Y})`，默认 `mode` 为 `lowpoly`，行为不变）：

```cpp
void modifyModeUsesModifyTemplate()
{
    MockLlmServer server({textChunk("把招牌材质换成深绿做旧铁皮，粗糙度调高")});
    LlmGateway llm;
    SessionStore sessions(QDir::tempPath());
    SettingsStore settings(QDir::tempPath());
    settings.update(agentSettings(server.baseUrl(), "k", "m", QJsonObject()));
    SummarizerAgent agent(&llm, &sessions, &settings);
    QSignalSpy ready(&agent, &SummarizerAgent::draftReady);

    agent.generate({QStringLiteral("blender"), QStringLiteral("招牌弄旧一点"),
                    QStringLiteral("modify"), QStringLiteral("sword_01"), {}});

    QTRY_COMPARE(ready.count(), 1);
    const QString system = server.requestAt(0).value("messages").toArray()
                               .first().toObject().value("content").toString();
    QVERIFY(system.contains(QStringLiteral("已经存在")));   // 修改模板
    QVERIFY(!system.contains(QStringLiteral("底座与构图"))); // 不是新建模板
    QVERIFY(system.contains(QStringLiteral("sword_01")));   // 资产名已注入
}

void discussionIsInjected()
{
    MockLlmServer server({textChunk("主题与视角：雨夜便利店")});
    LlmGateway llm;
    SessionStore sessions(QDir::tempPath());
    SettingsStore settings(QDir::tempPath());
    settings.update(agentSettings(server.baseUrl(), "k", "m", QJsonObject()));
    SummarizerAgent agent(&llm, &sessions, &settings);
    QSignalSpy ready(&agent, &SummarizerAgent::draftReady);

    agent.generate({QStringLiteral("blender"), QStringLiteral("做个便利店"),
                    QStringLiteral("lowpoly"), {},
                    QStringLiteral("已确认：要有自动门与积水")});

    QTRY_COMPARE(ready.count(), 1);
    const QString system = server.requestAt(0).value("messages").toArray()
                               .first().toObject().value("content").toString();
    QVERIFY(system.contains(QStringLiteral("用户已确认的讨论要点")));
    QVERIFY(system.contains(QStringLiteral("自动门与积水")));
}
```

`src/core/agent/SummarizerAgent.h` 改签名：

```cpp
struct SummarizeRequest
{
    QString area;
    QString userText;
    QString mode = QStringLiteral("lowpoly");
    QString asset;
    QString discussion;
};

void generate(const SummarizeRequest& request);
```

`SummarizerAgent.cpp` 的模板选择与注入：

```cpp
void SummarizerAgent::generate(const SummarizeRequest& request)
{
    ensurePromptResources();

    const LlmConfig cfg = resolveConfig();
    if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
        emit failed(request.area, QStringLiteral("agent-not-configured"));
        return;
    }

    // 三种模式三份模板：修改已有资产绝不能套用新建场景的骨架，否则 agent 会清场重建
    QString templatePath = QStringLiteral(":/prompts/summarizer.md");
    if (request.mode == QLatin1String("modify"))
        templatePath = QStringLiteral(":/prompts/summarizer-modify.md");
    else if (request.mode == QLatin1String("cloud3d"))
        templatePath = QStringLiteral(":/prompts/summarizer-cloud3d.md");

    QFile tpl(templatePath);
    QString system;
    if (tpl.open(QIODevice::ReadOnly | QIODevice::Text))
        system = QString::fromUtf8(tpl.readAll());
    if (system.isEmpty()) {
        emit failed(request.area, QStringLiteral("prompt-template-missing"));
        return;
    }
    system.replace(QStringLiteral("{{user_request}}"), request.userText);
    system.replace(QStringLiteral("{{mode}}"), request.mode);
    system.replace(QStringLiteral("{{asset}}"),
                   request.asset.isEmpty() ? QStringLiteral("（未指定）") : request.asset);
    // 讨论结论用代码拼接：模板里留空段会比留空行更难维护
    if (!request.discussion.isEmpty()) {
        system += QStringLiteral("\n\n## 用户已确认的讨论要点\n") + request.discussion;
    }

    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"), system}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), request.userText}}};

    auto buffer = std::make_shared<QString>();
    const QString area = request.area;
    m_llm->chatStream(
        cfg, messages, {},
        {.onDelta = [buffer](const QString& delta) { buffer->append(delta); },
         .onToolCalls = nullptr,
         .onError = [this, area](const QString& error) { emit failed(area, error); },
         .onDone = [this, area, buffer]() {
             // 落库交给 CoreApp 的 draftReady 接线：那里才知道 asset / source，并能拿到新行 id
             emit draftReady(area, *buffer);
         }});
}
```

`src/resources/prompts/summarizer-modify.md`：

```markdown
你是 3D 资产修改指令的改写助手。用户正在修改一个**已经存在**的低模资产，不是新建场景。

## 本次任务
- 目标资产：{{asset}}
- 用户需求：{{user_request}}

## 改写要求
把用户需求改写成一条明确、可执行的 Blender 修改指令：
1. 说清改哪个部分、改成什么（材质 / 几何 / 颜色 / 尺寸 / 位置）。
2. 只描述**改动**，不要重述整个场景，不要给建造计划，不要列物体清单。
3. 不要提导出、保存、文件名——这些由系统负责。
4. 用户没提到要改的部分一律保持原样。
5. 一到三句话，不分节，不加小标题。

## 输出格式
只输出改写后的指令：不要解释、不要寒暄、不要 markdown 代码块围栏。
```

Run: `ctest --test-dir build -C Debug -R tst_summarizer --output-on-failure`
Expected: 6 用例 Passed。

- [ ] **Step 4: ask 分支（CoreApp）**

`src/resources/prompts/ask.md`：

```markdown
你是 SynthForge Studio 的 3D 资产顾问，正在和用户讨论建模与改造方案。

## 当前上下文
- 当前选中资产：{{asset}}
- 资产状态：{{asset_state}}

## 资产库
{{assets}}

## 你的职责
1. 回答用户的问题，解释可行做法、代价与取舍，可以给多个方案并推荐一个。
2. 用户问「为什么预览里看不到某个效果」时，如实说明：粒子雨、材质动画、三渲二着色只存在于 .blend 中，不会进入 GLB。
3. 不要声称你已经修改了模型——你只讨论，不动手。
4. 目标资产没有 .blend 源文件时（云 API 生成 / 外部导入），说明只能重新生成。

## 可执行草稿
当讨论已经形成**具体的、可以直接动手**的方案时，在回答正文之后追加：

[[EXEC]]
（可直接执行的草稿）
[[/EXEC]]

块内写什么：
- 当前**有选中资产**：一到三句话，说清改哪个部分、改成什么；不要重述整个场景，不要提导出与保存。
- 当前**没有选中资产**：写一份可用的建模提示词，按下面的节标题组织，每节一句到三句。

主题与视角：
底座与构图：
场景主体：
细节内容：
美术风格：
动效（分别标注「可导出」的物体关键帧与「仅源文件」的粒子/自发光）：
产出结构要求（每个可辨识物品独立成物体，列出必须独立的清单）：
层级与命名：
轴心与摆放：
材质与贴图（贴图必须是图片文件，不超 1024×1024）：
数量下限（含可导出动效不少于 3 个）：
禁止项（无人物、无品牌 Logo、禁止片状几何体冒充粒子）：

只是解释原因、回答疑问、或方案还没定时，**不要**输出 [[EXEC]] 块。

## 输出格式
正文正常聊天。不要在正文里提到标记本身，不要说“我给出了执行块”。
```

`CoreApp.cpp` 顶部补 `#include "core/agent/AskReply.h"`、`#include <QFile>`、`#include <QFileInfo>`、`#include <QSet>`。

`CoreApp::handleChatSend` 在 `m_lastSend.insert` 之后、lowpoly 分支之前插入 ask 分支：

```cpp
    if (mode == QLatin1String("ask")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{QStringLiteral("area"), area}, {QStringLiteral("role"), "user"},
                            {QStringLiteral("content"), text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
        }

        const QString asset = params.value(QStringLiteral("asset")).toString();
        QFile tpl(QStringLiteral(":/prompts/ask.md"));
        QString system;
        if (tpl.open(QIODevice::ReadOnly | QIODevice::Text))
            system = QString::fromUtf8(tpl.readAll());
        const bool hasSource = !asset.isEmpty()
                               && QFileInfo::exists(m_library->absoluteFor(asset));
        system.replace(QStringLiteral("{{asset}}"),
                       asset.isEmpty() ? QStringLiteral("（未选中）") : asset);
        system.replace(QStringLiteral("{{asset_state}}"),
                       asset.isEmpty() ? QStringLiteral("无")
                                       : (hasSource ? QStringLiteral("已选中的低模资产")
                                                    : QStringLiteral("无 .blend 源文件，只能重新生成")));
        // 资产清单：让模型能说「你库里有三个招牌」而不是瞎猜；glb/fbx/blend 同基名只列一次
        QString assetLines;
        QSet<QString> seenBases;
        for (const QString& rel : m_library->scanAssets()) {
            const QFileInfo fi(m_library->absoluteFor(rel));
            if (fi.suffix().toLower() != QLatin1String("glb"))
                continue;
            const QString base = fi.completeBaseName();
            if (seenBases.contains(base))
                continue;
            seenBases.insert(base);
            const bool hasBlend =
                QFileInfo(fi.dir().filePath(base + QStringLiteral(".blend"))).exists();
            assetLines += QStringLiteral("- %1%2\n").arg(
                rel, hasBlend ? QStringLiteral("（有 .blend，可修改）")
                              : QStringLiteral("（无 .blend，只能重新生成）"));
            if (seenBases.size() >= 30)
                break;
        }
        system.replace(QStringLiteral("{{assets}}"),
                       assetLines.isEmpty() ? QStringLiteral("（空）") : assetLines);

        QJsonArray messages{
            QJsonObject{{"role", "system"}, {"content", system}}};
        for (const QJsonValue& value : m_sessions->messagePage(area, 20).messages) {
            const QJsonObject message = value.toObject();
            messages.append(QJsonObject{{"role", message.value("role")},
                                        {"content", message.value("content")}});
        }

        const QString taskId = QStringLiteral("ask-%1").arg(++m_seq);
        m_tasks->enqueue(Task{
            taskId, area, QString(),
            [this, area, cfg, messages, asset, text](const QString& tid, TaskManager* tm) {
                // 尾部缓冲：[[EXEC]] 块可能在任意 delta 边界被切开，先压住 32 字符
                auto buffer = std::make_shared<QString>();
                auto tail = std::make_shared<QString>();
                LlmGateway::StreamHandlers handlers;
                handlers.onDelta = [this, area, buffer, tail](const QString& delta) {
                    buffer->append(delta);
                    tail->append(delta);
                    constexpr int kKeep = 32;
                    if (tail->size() > kKeep) {
                        const QString part = tail->left(tail->size() - kKeep);
                        *tail = tail->right(kKeep);
                        m_ws->broadcastEvent(QStringLiteral("chat.delta"),
                                             QJsonObject{{"area", area}, {"text", part}});
                    }
                };
                handlers.onDone = [this, area, tid, tm, buffer, asset, text]() {
                    if (tm->isCancelled(tid)) {
                        m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                               QStringLiteral("cancelled"), {});
                        tm->finish(tid);
                        return;
                    }
                    const AskReply reply = parseAskReply(*buffer);
                    m_sessions->appendMessage(area, QStringLiteral("assistant"), reply.visible);
                    m_ws->broadcastEvent(
                        QStringLiteral("chat.message"),
                        QJsonObject{{"area", area}, {"role", "assistant"},
                                    {"content", reply.visible}});
                    // 讨论产出的草稿也是一行 draft：刷新后靠它重建草稿卡
                    if (!reply.executable.isEmpty()) {
                        const qint64 draftId = m_sessions->appendMessage(
                            area, QStringLiteral("draft"), reply.executable,
                            QJsonObject{{"state", "pending"}, {"origin", "discussion"},
                                        {"asset", asset}, {"source", text}});
                        m_ws->broadcastEvent(
                            QStringLiteral("draft.created"),
                            QJsonObject{{"area", area}, {"id", draftId},
                                        {"prompt", reply.executable},
                                        {"origin", "discussion"}, {"asset", asset}});
                    }
                    m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                           QStringLiteral("done"), {},
                                           QJsonObject{{"reply", reply.visible}});
                    tm->finish(tid);
                };
                handlers.onError = [this, area, tid, tm](const QString& error) {
                    if (tm->isCancelled(tid)) {
                        m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                               QStringLiteral("cancelled"), {});
                        tm->finish(tid);
                        return;
                    }
                    m_ws->broadcastEvent(QStringLiteral("error.card"),
                                         QJsonObject{{"area", area}, {"reason", error}});
                    tm->finish(tid);
                };
                m_llm->chatStream(cfg, messages, {}, handlers, area);
            }});
        return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
    }
```

注意：`ask` 不在 preflight 条件里，天然不触发；把 ask 分支放在 `m_lastSend.insert` 之后、lowpoly 分支之前即可。（Step 5 会把 `lowpoly` / `modify` 也从条件里摘掉，届时只剩 `cloud3d`。）

- [ ] **Step 5: 草稿卡绑定资产与执行分流**

`CoreApp.h` 加暂存与方法声明：

```cpp
QHash<QString, QString> m_pendingDraftAsset;   // area -> 本次草稿针对的资产
QHash<QString, QString> m_pendingDraftSource;  // area -> 用户原始需求（供「重新生成」）
QHash<QString, qint64> m_regenDraft;           // area -> 正在重新生成的草稿 id
QJsonValue startModify(const QString& area, const QString& asset, const QString& text);
```

`handleChatSend` 开头那段 `if (mode == lowpoly || cloud3d || modify) { preflight… }` 先拆：**删掉 `lowpoly` 与 `modify` 两项，只留 `cloud3d`**（P5 未实现，仍需早拦）。

两个分支都改成「先记下来源，再交给 SummarizerAgent」，草稿由 `draftReady` 统一落库：

```cpp
    if (mode == QLatin1String("lowpoly")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            // …同现状：error.card + accepted=false
        }
        m_pendingDraftAsset.insert(area, QString());
        m_pendingDraftSource.insert(area, text);
        m_summarizer->generate({area, text}); // 默认 mode=lowpoly
        return QJsonValue(QJsonObject{{"accepted", true}});
    }

    if (mode == QLatin1String("modify")) {
        const QString asset = params.value(QStringLiteral("asset")).toString();
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        // 执行模式也先过一遍草稿卡：模糊指令由修改模板收敛，用户还能改措辞
        m_pendingDraftAsset.insert(area, asset);
        m_pendingDraftSource.insert(area, text);
        m_summarizer->generate({area, text, QStringLiteral("modify"),
                                QFileInfo(asset).completeBaseName(), {}});
        return QJsonValue(QJsonObject{{"accepted", true}});
    }
```

`draftReady`：入库并带回 id；若是「重新生成」触发的，改走 update：

```cpp
connect(m_summarizer, &SummarizerAgent::draftReady, this,
        [this](const QString& area, const QString& prompt) {
            // 重新生成：同一条草稿原地换文本，不新增卡片
            if (const qint64 regenId = m_regenDraft.take(area); regenId > 0) {
                m_sessions->updateMessageContent(regenId, prompt);
                m_ws->broadcastEvent(
                    QStringLiteral("draft.updated"),
                    QJsonObject{{"area", area}, {"id", regenId}, {"prompt", prompt}});
                return;
            }
            const QString asset = m_pendingDraftAsset.take(area);
            const QString source = m_pendingDraftSource.take(area);
            const QString mode = asset.isEmpty() ? QStringLiteral("lowpoly")
                                                 : QStringLiteral("modify");
            const qint64 id = m_sessions->appendMessage(
                area, QStringLiteral("draft"), prompt,
                QJsonObject{{"state", "pending"}, {"origin", "generated"},
                            {"mode", mode}, {"asset", asset}, {"source", source}});
            m_ws->broadcastEvent(
                QStringLiteral("draft.created"),
                QJsonObject{{"area", area}, {"id", id}, {"prompt", prompt},
                            {"origin", "generated"}, {"mode", mode}, {"asset", asset}});
        });
```

四个草稿请求（全部按 id 定位；原 `draft.cancel` 那段 `saveDraft("")` 整段删掉）：

```cpp
    m_router->registerHandler(QStringLiteral("draft.save"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QString content = params.value(QStringLiteral("content")).toString();
        if (id <= 0)
            return QJsonValue(QJsonObject{{"ok", false}});
        // 编辑失焦即存：刷新后看到的是改过的版本
        return QJsonValue(
            QJsonObject{{"ok", m_sessions->updateMessageContent(id, content)}});
                              });
    m_router->registerHandler(QStringLiteral("draft.send"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QString prompt = params.value(QStringLiteral("prompt")).toString().trimmed();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty() || prompt.isEmpty())
            return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "invalid-draft"}});

        const QString area = row.value(QStringLiteral("area")).toString();
        const QString asset = row.value(QStringLiteral("meta")).toObject()
                                  .value(QStringLiteral("asset")).toString();

        // 草稿阶段不拦 Blender（写方案只需 LLM）；点执行才检查，没开就弹提示且不占门禁
        const PreflightResult pre = m_preflight->checkLowpoly();
        if (!pre.ok) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", pre.reason}});
            return QJsonValue(QJsonObject{{"accepted", false}, {"reason", pre.reason}});
        }

        // 先确认源文件在，再转正：否则草稿卡消失了但执行失败，用户无法回头改
        if (!asset.isEmpty()) {
            QFileInfo src(m_library->absoluteFor(asset));
            if (src.suffix().toLower() != QLatin1String("blend"))
                src = QFileInfo(src.dir().filePath(src.completeBaseName()
                                                   + QStringLiteral(".blend")));
            if (!src.exists())
                return QJsonValue(QJsonObject{{"accepted", false},
                                              {"error", "no-blend-source"}});
        }
        // 草稿转正：改角色而不是新增 user 消息，否则历史里同一段文本出现两次
        m_sessions->updateMessageContent(id, prompt);
        m_sessions->setMessageRole(id, QStringLiteral("user"));
        m_ws->broadcastEvent(QStringLiteral("draft.done"),
                             QJsonObject{{"area", area}, {"id", id}});
        m_ws->broadcastEvent(QStringLiteral("chat.message"),
                             QJsonObject{{"area", area}, {"role", "user"},
                                         {"content", prompt}});

        if (!asset.isEmpty())
            return startModify(area, asset, prompt);
        m_orch->setModelConfig(primaryAgentConfig());
        m_orch->setConversationHistory(m_sessions->recentMessages(area, 20));
        m_orch->plan(QStringLiteral("plan-%1").arg(++m_seq), nullptr, prompt, area);
        return QJsonValue(QJsonObject{{"accepted", true}});
                              });
    m_router->registerHandler(QStringLiteral("draft.cancel"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty())
            return QJsonValue(QJsonObject{{"ok", false}});
        QJsonObject meta = row.value(QStringLiteral("meta")).toObject();
        meta.insert(QStringLiteral("state"), QStringLiteral("cancelled"));
        m_sessions->updateMessageMeta(id, meta);
        m_ws->broadcastEvent(
            QStringLiteral("draft.done"),
            QJsonObject{{"area", row.value(QStringLiteral("area")).toString()},
                        {"id", id}, {"cancelled", true}});
        return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("draft.regenerate"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty())
            return QJsonValue(QJsonObject{{"accepted", false}});

        const QString area = row.value(QStringLiteral("area")).toString();
        const QJsonObject meta = row.value(QStringLiteral("meta")).toObject();
        const QString asset = meta.value(QStringLiteral("asset")).toString();
        const QString source = meta.value(QStringLiteral("source")).toString();
        // 讨论产出的正文本身就是讨论结论；SummarizerAgent 产出的不能再当讨论喂回去
        const QString discussion =
            meta.value(QStringLiteral("origin")).toString() == QLatin1String("discussion")
                ? row.value(QStringLiteral("content")).toString()
                : QString();

        m_regenDraft.insert(area, id);
        m_summarizer->generate({area, source.isEmpty() ? discussion : source,
                                asset.isEmpty() ? QStringLiteral("lowpoly")
                                                : QStringLiteral("modify"),
                                QFileInfo(asset).completeBaseName(), discussion});
        return QJsonValue(QJsonObject{{"accepted", true}});
                              });
```

`startModify`：Task 6 的 `modify` 分支原样搬进来（`chat.send` 不再保留这段），只是不再自己落用户消息（`draft.send` 已经落过）：

```cpp
QJsonValue CoreApp::startModify(const QString& area, const QString& asset, const QString& text)
{
    // 修改锚点统一是 .blend：传 glb/fbx 时自动找同目录同名源文件
    QFileInfo info;
    if (!asset.isEmpty()) {
        info = QFileInfo(m_library->absoluteFor(asset));
        if (info.suffix().toLower() != QLatin1String("blend"))
            info = QFileInfo(info.dir().filePath(info.completeBaseName()
                                                 + QStringLiteral(".blend")));
    }
    const QString blendRelative =
        info.exists() ? m_library->relativeFor(info.absoluteFilePath()) : QString();
    if (asset.isEmpty() || text.isEmpty() || blendRelative.isEmpty()
        || !m_library->isValidTarget(blendRelative))
        return QJsonValue(QJsonObject{{"accepted", false}, {"error", "no-blend-source"}});

    BuildPlan plan;
    plan.groups.append(
        BuildGroup{QStringLiteral("修改资产"), {},
                   QStringLiteral("修改意图达成；同名 GLB/FBX/blend 已覆盖")});
    plan.maxSteps = 50;
    const QString prompt = QStringLiteral("打开 .blend 源文件 %1。要求：%2。"
                                          "保留场景里已有的可导出动效（物体关键帧），不要删除或覆盖。"
                                          "完成后覆盖导出同名 GLB/FBX 并保存 .blend。")
                               .arg(info.absoluteFilePath(), text);
    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);
    const QString targetFolder = info.absolutePath();
    m_tasks->enqueue(
        Task{taskId, area, QString(),
             [this, plan, prompt, targetFolder, area](const QString& tid, TaskManager* tm) {
                 m_orch->setModelConfig(primaryAgentConfig());
                 m_orch->setConversationHistory(m_sessions->recentMessages(area, 20));
                 m_orch->execute(tid, tm, ToolBridge::mcpToolsToOpenAi(m_blenderTools),
                                 plan, prompt, targetFolder, area);
             }});
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}
```

- [ ] **Step 6: 前端开关与三按钮草稿卡**

> 至此 `CoreApp.cpp` 里两处 `saveDraft`（原 `draft.send` / `draft.cancel`）已随新 handler 消失，全库不再引用 `drafts` 表。

`ChatPanel.vue`：输入区左侧加分段开关，发送时按开关与 `modifyTarget` 决定 mode：

```vue
<div class="mode-switch">
  <button :class="{ active: chatMode === 'ask' }" @click="chatMode = 'ask'">讨论</button>
  <button :class="{ active: chatMode === 'exec' }" @click="chatMode = 'exec'">执行</button>
</div>
```

```ts
const chatMode = ref<"ask" | "exec">("exec");

async function send() {
  const text = input.value.trim();
  if (!text || busy.value)
    return;
  busy.value = true;
  const mode = chatMode.value === "ask"
    ? "ask"
    : (props.modifyTarget ? "modify" : "lowpoly");
  await client.request("chat.send", {
    area: props.area, mode, text,
    asset: props.modifyTarget ?? undefined,
  });
  input.value = "";
}
```

订阅三条草稿事件（替掉现有的 `draft.created` 处理）：

```ts
props.client.on("draft.created", (data) => {
  const d = data as { area?: string; id?: number; prompt?: string; origin?: string };
  if (d.area !== props.area) return;
  busy.value = false;
  messages.value.push({
    role: "draft",
    id: d.id,
    content: d.prompt ?? "",
    origin: d.origin === "discussion" ? "discussion" : "generated",
    state: "pending",
  });
  scrollToBottom();
}),
props.client.on("draft.updated", (data) => {
  const d = data as { area?: string; id?: number; prompt?: string };
  const card = messages.value.find((m) => m.role === "draft" && m.id === d.id);
  if (card) card.content = d.prompt ?? "";
}),
props.client.on("draft.done", (data) => {
  const d = data as { area?: string; id?: number; cancelled?: boolean };
  const card = messages.value.find((m) => m.role === "draft" && m.id === d.id);
  if (!card) return;
  if (d.cancelled)
    card.state = "cancelled"; // 灰卡只读
  else
    messages.value = messages.value.filter((m) => m !== card); // 已转成 user 气泡，避免重复
}),
```

`DraftCard.vue`：三按钮 + 失焦落库：

```vue
<div class="draft-actions" v-if="state === 'pending'">
  <button @click="run">执行</button>
  <button @click="regenerate">重新生成</button>
  <button @click="cancel">取消</button>
</div>
```

```ts
const props = defineProps<{
  id?: number;
  content: string;
  client: WsClient;
  state?: "pending" | "cancelled";
  origin?: "discussion" | "generated";
}>();

const text = ref(props.content);
watch(() => props.content, (v) => { text.value = v; }); // 重新生成后回填

// 退出编辑即落库：刷新后看到的是改过的版本
async function save() {
  if (props.state !== "pending" || !props.id || text.value === props.content)
    return;
  await props.client.request("draft.save", { id: props.id, content: text.value });
}

async function run() {
  const res: any = await props.client.request("draft.send", {
    id: props.id, prompt: text.value,
  });
  if (res?.accepted === false)
    error.value = res.reason ?? "执行失败";
}

function regenerate() {
  props.client.request("draft.regenerate", { id: props.id });
}

async function cancel() {
  await props.client.request("draft.cancel", { id: props.id });
}
```

`history.ts`：`toChatItems` 增加草稿分支，靠 meta 还原刷新后的卡片：

```ts
if (role === "draft") {
  const meta = (item.meta ?? {}) as Record<string, unknown>;
  return {
    role: "draft",
    id: item.id,
    content: item.content,
    origin: meta.origin === "discussion" ? "discussion" : "generated",
    state: meta.state === "cancelled" ? "cancelled" : "pending",
  };
}
```

`ChatItem` 增加 `id?: number`、`origin?: "discussion" | "generated"`、`state?: "pending" | "cancelled"`。

- [ ] **Step 7: CMake + 全量测试 + Commit**

`CMakeLists.txt`：

```cmake
qt_add_resources(synthforge_core "prompts"
    PREFIX "/prompts"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer-modify.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/ask.md")

add_executable(tst_ask_reply tests/tst_ask_reply.cpp)
target_link_libraries(tst_ask_reply PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_ask_reply COMMAND tst_ask_reply)
set_tests_properties(tst_ask_reply PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
```

`src/core/agent/AskReply.cpp` 加进 `synthforge_core` 源列表。

Run: `cmake --build build --config Debug` 然后 `ctest --test-dir build -C Debug -LE slow --output-on-failure`
Expected: 全绿，`tst_ask_reply` 4 用例、`tst_summarizer` 6 用例。

手工验收：
1. 输入框切「讨论」→ 发「这个招牌怎么改才好看」→ 只有文字回答，Blender 未连接也能聊。
2. 继续讨论到方案明确 → 回答下方出现草稿卡（内容即模型给的草稿）；点「重新生成」→ 卡片内容被 SummarizerAgent 重写。
3. 点「执行」：无选中资产 → 计划卡；有选中资产 → 直接开干（无计划卡、无目录选择）。
4. 切「执行」→ 选中有 `.blend` 的资产 → 发「弄旧一点」→ 先出草稿卡（修改模板），点执行才动 Blender。
5. 选中云 API 资产（无 `.blend`）→ 讨论时模型说明只能重新生成；草稿卡点执行返回 `no-blend-source`。
6. 改草稿文字 → 点空白处失焦 → 刷新页面 → 卡片回到原位且显示改过的文字。
7. 取消一张草稿后刷新 → 灰卡只读，无按钮；执行过的那条变成普通 user 气泡，历史里不重复。
8. 关掉 Blender 插件（或退出 Blender）→ 执行模式发「弄旧一点」→ **仍出草稿卡**；点执行 → 立刻弹 `blender-not-ready`，卡片保留可继续改。
9. 讨论模式问「我库里都有什么」→ 回答能列出 `{{assets}}` 里的文件名，并区分「可修改」与「无 .blend」。

```bash
git add CMakeLists.txt src/core/agent/AskReply.h src/core/agent/AskReply.cpp \
  src/core/agent/SummarizerAgent.h src/core/agent/SummarizerAgent.cpp \
  src/resources/prompts/ask.md src/resources/prompts/summarizer-modify.md \
  src/core/coreapp/CoreApp.h src/core/coreapp/CoreApp.cpp \
  src/core/store/SessionStore.h src/core/store/SessionStore.cpp \
  tests/tst_ask_reply.cpp tests/tst_summarizer.cpp tests/tst_session_store.cpp \
  web/src/components/ChatPanel.vue web/src/components/DraftCard.vue web/src/chat/history.ts
```

Commit（`feat: 讨论/执行双模式与统一草稿卡`）。

---

## P4 完成标准

- [ ] 低模全链真实跑通（发送 → 草稿 → 计划卡确认 → 确认目录 → 分组建模/自检 → 截图 → 落盘 → 预览）
- [ ] 修改已有资产路径跑通：先出修改草稿卡，确认后无计划卡、无位置弹窗
- [ ] 讨论模式全程不碰 Blender：不 preflight、不占 `BlenderGate`，Blender 未启动时也能正常对话
- [ ] 讨论收敛后回答下方出现草稿卡（`[[EXEC]]` 标记对用户不可见，流式过程中也不闪现）；可编辑、可重新生成（走 SummarizerAgent）、可执行
- [ ] 草稿卡点执行按有无资产分流：有资产直接 `modify`，无资产进 `plan()`；无 `.blend` 源文件时返回 `no-blend-source` 且不动 Blender
- [ ] `draft.regenerate` 按资产有无选择 `modify` / `lowpoly` 模板，并把讨论结论注入 system
- [ ] 草稿卡落库在 `messages.meta`（不再用 `drafts` 表）：刷新/重启后按 `id` 原位恢复；手动编辑失焦即存；点执行后同一行转成 `role='user'`（历史不重复），取消后变灰只读
- [ ] `tst_ask_reply`（4 用例：无标记 / 正常切分 / 未闭合 / 空块）与 `tst_summarizer`（6 用例，含 modify 模板不串新建骨架、讨论结论已注入）全绿
- [ ] 规划解析失败自动降级默认四分组，流程不中断；`plan.cancel` 正确取消任务
- [ ] 每个新建任务开始前确定性清场（MCP 直调，不经 LLM）：连做两个任务，第二个 `.blend` 里不含第一个任务的物体；清场失败以 `clear-scene-failed` 终止，不带着旧场景继续建
- [ ] 导出三件套同基名；目标目录已有同名文件时自动递增为 `name_2`，旧资产不被覆盖，最终回复告知实际文件名
- [ ] 草稿阶段不前置检查：Blender 未连接时仍能生成草稿；点执行（`draft.send`）时立即弹 `blender-not-ready` 且不占门禁、不进计划/执行链
- [ ] Blender 低模 / 修改任务全程持有 `BlenderGate`（owner=`blender-agent`）；成功 / 失败 / 取消三个出口均释放；门禁忙时 `failed("blender-busy")` 且零 LLM / MCP 调用
- [ ] `plan.confirm` 在门禁忙时返回 `blender-busy` 且**不消费** `m_pendingPlans`（计划卡保留，可稍后再点开始）
- [ ] 截图压缩后回喂（长边 ≤1024、JPEG q80），且同一截图以 `chat.image` 事件在对话区渲染为图片卡
- [ ] 取消任务后 MCP 调用在工具边界停止，任务事件 `finished(cancelled=true)`；草稿/规划/计划/执行四个可等待阶段的「停止」均生效（规划丢弃迟到结果不弹卡）
- [ ] `tst_preflight`、`tst_mcp_runtime`、`tst_blender_gate`、`tst_asset_library`、`tst_orchestrator`、`tst_llm_gateway`、`tst_ask_reply` 全绿（`tst_orchestrator` 6 用例，含门禁忙拒绝与成功释放；`tst_llm_gateway` 7 用例走真实 HTTP+SSE，删掉 `LlmGateway` 的 `readyRead`/`finished` 分支必须变红）
- [ ] `fs.operate` 五项操作全部可用（mkdir/rename/delete/reveal/refresh），越界路径返回 `invalid-path`；`folderPick.mkdir` 可在弹窗内新建文件夹
- [ ] `file.read` 支持文本 / 图片 / GLB / 模型文件信息卡四类，模型文件返回 `blendPath`；`asset.created` 后预览区自动加载新资产；未选中时显示概览卡
- [ ] 资产树平铺 glb/fbx/blend 三种文件；点 glb 出 3D 预览，点 fbx/blend 出文件信息卡；点文件**不**产生修改 chip
- [ ] [修改此资产] 按钮与右键菜单都能进入修改模式（chip 指向 `.blend`）；无 `.blend` 时弹窗说明只能重新生成，且不发 `chat.send`
- [ ] glb 自带的物体关键帧动画全部自动循环播放，底部动画条显示播放 / 暂停与各段时长，无动画时不显示；相机按包围球自动定位
- [ ] 预览区右上角常驻「完整材质、动画与特效请在 Blender 中查看」
- [ ] 修改资产后原可导出动效仍在（导出命令带 `export_animations=True` 且 agent 未删关键帧）
