# SynthForge Studio P7：打磨与打包实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 收尾打磨：统一日志（按天分文件 + `log.append` 诊断事件）、启动环境自检与错误卡字典（含重试/重启桥接动作）、双灯状态细化（连接中/重连中/就绪/失败原因）、Release 打包脚本（windeployqt 产出可分发目录）与整机冒烟清单。

**Architecture:** `LogService` 用 `qInstallMessageHandler` 接管全部 Qt 日志写文件并按天轮转，warning 以上经事件 sink 转为 `log.append` 并只在 WS 线程广播；`EnvChecker` 异步检查 uvx 可用性与两条 MCP 链路状态，启动自动运行一次并广播 `env.status`；前端 `ErrorCard` 统一渲染原因字典并接 `chat.retry` / `bridge.restart`；打包为 CMake install + windeployqt 脚本，产物放 `dist/`。

**规格映射：** 9 错误处理、4.2 双灯、4.6 对话区、8.3 事件族（`log.append`/`env.status`）、8.8 环境自检、7.3 日志、11 阶段 7（打磨打包）。

**依赖：** P2~P6 全部。

## Global Constraints

- 日志目录 `%APPDATA%/SynthForgeStudio/logs/`，文件名 `yyyy-MM-dd.log`；单文件超 50MB 时轮转为 `<date>-1.log` 并继续；跨天时轮转序号归零。
- `log.append` 只转发 `warning` / `critical` / `fatal` 级别（`info` / `debug` 仅落盘），载荷固定 `{level:"warning"|"error", category, message, time}`；前端环形缓冲最多保留 200 条。
- 错误卡原因字典与 `error.card.reason` 枚举一一对应（见 Task 2 表），新增 reason 必须同步前后端；未收录的 reason 显示 `message`，无 `message` 时显示 reason 原文（不得出现空白卡）。
- 错误卡动作只有三类：`[重试]` → `chat.retry {area}`、`[重启桥接]` → `bridge.restart {area}`、`[打开设置]`；按 Task 2 表的动作列渲染。
- 环境自检不得阻塞启动：并行发起、5 秒单项超时、结果以 `env.status` 事件推送（`env.check` 仅触发检查并立即返回 `{ok:true}`）。
- 打包产物必须可在**未安装 Qt 的 Windows 10/11 机器**直接运行（WebEngine 全家桶齐全）。

## File Structure

```
src/core/logging/LogService.h/.cpp      ← 文件日志接管 + 轮转 + log.append 事件源
src/core/env/EnvChecker.h/.cpp          ← uvx/Blender/Unity 自检
web/src/components/ErrorCard.vue        ← 错误卡组件 + 原因字典 + 三类动作
web/src/components/LogDrawer.vue        ← log.append 抽屉 + 环境自检结果
web/src/components/TopBar.vue           ← Modify：hints 悬停、日志入口
scripts/package.ps1                     ← Release 打包脚本
tests/tst_env_checker.cpp
```

---

### Task 1: LogService（按天日志 + 轮转）

**Files:** Create `src/core/logging/LogService.h/.cpp`；Modify `src/main.cpp` 与 `src/cli/main.cpp`（启动即安装）、`src/core/coreapp/CoreApp.cpp`（注入事件 sink）。

**Interfaces:**
- `class LogService { static void install(const QString& logDir); static QString currentLogFile(); static void shutdown(); static void setEventSink(QObject* context, std::function<void(const QJsonObject&)> sink); }`
- 行为：接管 `qInstallMessageHandler`；每行格式 `[HH:mm:ss.zzz] [D|I|W|E] category: message`；同时写文件与 stderr；达到 50MB 轮转；`QMutex` 保护多线程写入。
- 事件桥：`setEventSink` 在 `CoreApp::start()` 末尾注入；仅 `QtWarningMsg/QtCriticalMsg/QtFatalMsg` 入队（`Qt::QueuedConnection` 回主线程）；sink 内日志不得再次入队（`thread_local` 防环）。

- [ ] **Step 1: 实现**

```cpp
namespace {
QFile* g_file = nullptr;
QString g_dir;
QDate g_date;
QString g_path;
int g_rotate = 0;
QMutex g_mutex;
QPointer<QObject> g_sinkContext;
std::function<void(const QJsonObject&)> g_sink;
thread_local bool g_inSink = false;

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QDate today = QDate::currentDate();
    const QString wanted = g_dir + "/" + today.toString("yyyy-MM-dd")
                           + (g_rotate ? QString("-%1").arg(g_rotate) : QString()) + ".log";
    const char level = type == QtDebugMsg    ? 'D'
                       : type == QtInfoMsg   ? 'I'
                       : type == QtWarningMsg ? 'W'
                                            : 'E';
    const QString line = QStringLiteral("[%1] [%2] %3: %4\n")
                             .arg(QTime::currentTime().toString("HH:mm:ss.zzz"))
                             .arg(QLatin1Char(level))
                             .arg(QString::fromUtf8(context.category ? context.category : "app"),
                                  message);

    {
        QMutexLocker lock(&g_mutex);
        if (!g_file || g_date != today || g_path != wanted) {
            delete g_file;
            g_file = nullptr;
            if (g_date != today)
                g_rotate = 0;                     // 跨天：当天轮转序号归零
            g_date = today;
            g_path = wanted;
            QDir().mkpath(g_dir);
            g_file = new QFile(g_path);
            g_file->open(QIODevice::Append | QIODevice::WriteOnly);
        }
        if (g_file->size() > 50 * 1024 * 1024) {  // 轮转后重开，本条写入新文件
            g_file->close();
            delete g_file;
            ++g_rotate;
            g_path = g_dir + "/" + today.toString("yyyy-MM-dd")
                     + QString("-%1").arg(g_rotate) + ".log";
            g_file = new QFile(g_path);
            g_file->open(QIODevice::Append | QIODevice::WriteOnly);
        }
        if (g_file)
            g_file->write(line.toUtf8());
        if (g_file)
            g_file->flush();
    }
    fprintf(stderr, "%s", line.toUtf8().constData());

    // P7：warning 及以上转发为 log.append（info/debug 仅落盘，避免日志洪水）
    if (!g_sink || !g_sinkContext || g_inSink)
        return;
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg)
        return;
    const QJsonObject entry{
        {"level", type == QtWarningMsg ? "warning" : "error"},
        {"category", QString::fromUtf8(context.category ? context.category : "app")},
        {"message", message},
        {"time", QTime::currentTime().toString("HH:mm:ss")}};
    QMetaObject::invokeMethod(
        g_sinkContext,
        [entry]() {
            if (!g_sink)
                return;
            g_inSink = true;                  // sink 内再打日志不得重新入队，防环
            g_sink(entry);
            g_inSink = false;
        },
        Qt::QueuedConnection);
}
} // namespace

void LogService::install(const QString& logDir)
{
    g_dir = logDir;
    qInstallMessageHandler(handler);
}

QString LogService::currentLogFile()
{
    QMutexLocker lock(&g_mutex);
    return g_path;
}

void LogService::setEventSink(QObject* context, std::function<void(const QJsonObject&)> sink)
{
    g_sinkContext = context;
    g_sink = std::move(sink);
}

void LogService::shutdown()
{
    qInstallMessageHandler(nullptr);
    g_sink = nullptr;
    g_sinkContext = nullptr;
    QMutexLocker lock(&g_mutex);
    delete g_file;
    g_file = nullptr;
}
```

```cpp
// LogService.h
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class LogService
{
public:
    static void install(const QString& logDir);
    static QString currentLogFile();
    static void shutdown();
    static void setEventSink(QObject* context, std::function<void(const QJsonObject&)> sink);
};
```

> `.cpp` 需包含 `<QDate> <QDir> <QFile> <QMutex> <QMutexLocker> <QPointer> <QTime>`。`setEventSink` 必须在 `QCoreApplication` 构造之后调用；`shutdown()` 后 handler 移除，后续日志回到默认 stderr。

- [ ] **Step 2: 接线（日志文件 + log.append 事件源）**

`main.cpp` / `cli/main.cpp` 开头（设置 QCoreApplication 名称后）调用 `LogService::install(defaultDataDir() + "/logs")`；`qInfo()` 打印版本与启动参数；退出前调用 `LogService::shutdown()`。

`CoreApp::start()` 末尾（`m_ws->start()` 之后）注入事件 sink：

```cpp
LogService::setEventSink(this, [this](const QJsonObject& entry) {
    m_ws->broadcastEvent("log.append", entry);
});
```

> `log.append` 不缓存历史：WS 客户端连接前的日志不回放（日志文件才是真相源）。前端抽屉只展示连接后的 warning/error。

- [ ] **Step 3: 验证**：启动 app → 日志文件生成；消息同时出现在文件与 stderr。Commit（`feat: 按天文件日志与轮转`）。

---

### Task 2: EnvChecker + 双灯细化 + 错误卡字典

**Files:** Create `src/core/env/EnvChecker.h/.cpp`、`web/src/components/ErrorCard.vue`、`web/src/components/LogDrawer.vue`；Modify `CoreApp`（启动时 `QTimer::singleShot(0, …)` 异步自检；新增 `env.check` handler、`env.status` 广播）、`web/src/components/TopBar.vue`（hints + 日志入口）、`web/src/App.vue`（env.status/log.append 订阅）、`web/src/components/ChatPanel.vue`（ErrorCard 接线）。

**Interfaces:**
- `struct EnvItem { QString state; /* unknown|checking|ready|failed */ QString detail; }`
- `class EnvChecker : QObject { EnvChecker(QString uvxPath, McpRuntime* blender, McpRuntime* unity, QObject* parent = nullptr); void run(); signals: void result(QJsonObject /* {uvx, blender, unity} */); }`
  - `uvx`：`QProcess` 执行 `uvx --version`，5s 超时；成功回 `{state:"ready", detail:"uvx x.y.z"}`。
  - `blender` / `unity`：对应 `McpRuntime::isReady()` + `tools().size()`；未连接回 `{state:"failed", detail:"blender-not-ready"}` / `"unity-not-ready"`。
- `env.check` 请求：立即返回 `{ok:true}` 并触发 `run()`；结果异步以 `env.status {uvx, blender, unity}` 事件广播（不阻塞 WS 线程）。
- `conn.status` 事件载荷：`{blender:"off|ready|reconnecting", unity:"...", hints:{blender?:detail, unity?:detail}}`（P4 `broadcastConnStatus` 已实现，P7 只消费 `hints`）；P4/P6 的 `statusChanged` 接线在「曾就绪后变 off」时广播 `error.card {reason:"bridge-crashed", message:hint}`（主动 stop 与首次启动失败不报）。`off` 的来源：P1 `McpClient::onTransportStopped` 在意外退出（含心跳超时）时 `emit failed(...)`，`McpRuntime` 映射为 `off`；`m_crashNotified` 保证一次断链只出卡一次，恢复 `ready` 后才允许下次再报。
- `error.card` 原因字典（前后端同步，15 条 + 兜底）：

| reason | 展示文案 | 动作 |
|---|---|---|
| `blender-not-ready` | Blender 链路未就绪：请打开 Blender 并启动 MCP 插件 | 重试 / 重启桥接 |
| `unity-not-ready` | Unity 链路未就绪：请导入工程并确认 MCP 包运行 | 重试 / 重启桥接 |
| `agent-not-configured` | Agent 模型未配置：设置 → Agent 模型 | 打开设置 |
| `cloud3d-not-configured` | 3D 服务未配置：设置 → 3D 模型 | 打开设置 |
| `invalid-project` | 不是有效 Unity 工程（需含 Assets 与 ProjectSettings） | 无 |
| `cloud3d-failed` | 云 3D 任务失败（附服务端 message） | 重试 |
| `user-cancelled` | 已取消 | 无 |
| `bridge-crashed` | MCP 桥接异常退出，正在自动重连 | 重启桥接 |
| `project-not-active` | 未激活 Unity 工程：请先在顶部导入工程 | 无 |
| `invalid-attachment` | 附件无效：文件不存在或不在资产库内 | 无 |
| `blender-convert-failed` | glb→fbx 转换失败：请查看 Blender 日志后重试 | 重试 / 重启桥接 |
| `import-failed` | 导入 Unity 失败：请查看 Unity Console 后重试 | 重试 |
| `max-steps-exceeded` | Agent 步数超限，任务已停止 | 重试 |
| `empty-plan` | 建造计划为空（规划结果无分组）：请重试或补充需求描述 | 重试 |
| `bridge-not-registered` | 当前区未注册桥接，无法重启（P6 未接入或配置错误） | 无 |
| （未收录） | 显示 `message`；无 `message` 时显示 reason 原文 | 重试 |

- [ ] **Step 1: 写失败测试**

```cpp
#include <QtTest>
#include "core/env/EnvChecker.h"

class EnvCheckerTest : public QObject
{
    Q_OBJECT
private slots:
    void reportsUvxUnavailableGracefully()
    {
        EnvChecker checker(QStringLiteral("definitely-not-a-real-binary-xyz"), nullptr, nullptr);
        QSignalSpy spy(&checker, &EnvChecker::result);
        checker.run();
        QVERIFY(spy.wait(8000));
        const QJsonObject report = spy.first().first().toJsonObject();
        QCOMPARE(report.value("uvx").toObject().value("state").toString(),
                 QStringLiteral("failed"));
        QCOMPARE(report.value("blender").toObject().value("state").toString(),
                 QStringLiteral("failed"));
    }
};

QTEST_MAIN(EnvCheckerTest)
#include "tst_env_checker.moc"
```

- [ ] **Step 2: 实现 EnvChecker 核心**

```cpp
void EnvChecker::run()
{
    auto report = std::make_shared<QJsonObject>();
    auto done = std::make_shared<bool>(false);
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QTimer::singleShot(5000, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
    auto finish = [this, proc, report, done](int code) {
        if (*done)
            return;                       // finished 与 errorOccurred 只取先到者
        *done = true;
        (*report)["uvx"] = QJsonObject{
            {"state", code == 0 ? "ready" : "failed"},
            {"detail", QString::fromUtf8(proc->readAll()).trimmed()}};
        (*report)["blender"] =
            m_blender && m_blender->isReady()
                ? QJsonObject{{"state", "ready"},
                              {"detail", QStringLiteral("已就绪 · %1 个工具")
                                             .arg(m_blender->tools().size())}}
                : QJsonObject{{"state", "failed"}, {"detail", "blender-not-ready"}};
        (*report)["unity"] =
            m_unity && m_unity->isReady()
                ? QJsonObject{{"state", "ready"},
                              {"detail", QStringLiteral("已就绪 · %1 个工具")
                                             .arg(m_unity->tools().size())}}
                : QJsonObject{{"state", "failed"}, {"detail", "unity-not-ready"}};
        emit result(*report);
        proc->deleteLater();
    };
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus) { finish(code); });
    connect(proc, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError) { finish(-1); });   // 可执行文件不存在也会出结果
    proc->start(m_uvxPath, {QStringLiteral("--version")});
}
```

`EnvChecker.h`：

```cpp
class EnvChecker : public QObject
{
    Q_OBJECT
public:
    EnvChecker(QString uvxPath, McpRuntime* blender, McpRuntime* unity,
               QObject* parent = nullptr);
    void run();

signals:
    void result(const QJsonObject& report);

private:
    QString m_uvxPath;
    McpRuntime* m_blender;
    McpRuntime* m_unity;
};
```

`CoreApp::start()`（`m_unity` 创建之后）：

```cpp
m_env = new EnvChecker(m_settings->data().value("uvxPath").toString("uvx"),
                       m_blender, m_unity, this);
connect(m_env, &EnvChecker::result, this, [this](const QJsonObject& report) {
    m_ws->broadcastEvent("env.status", report);
});
m_router->registerHandler("env.check", [this](const QJsonObject&) {
    QTimer::singleShot(0, m_env, &EnvChecker::run);   // 不阻塞 WS 线程
    return QJsonValue(QJsonObject{{"ok", true}});
});
QTimer::singleShot(0, m_env, &EnvChecker::run);       // 启动自动自检一次
```

> `m_env` 与 `env.check` handler 在 `m_unity` 创建（P6 Task 3）之后接线；`EnvChecker` 构造时保存指针即可，`run()` 时再读 `isReady()`，因此启动阶段未握手不会误判为失败（建议 `QTimer::singleShot(1500, …)` 再跑首次自检）。

- [ ] **Step 3: 前端错误卡、双灯与日志抽屉**

`ErrorCard.vue`（字典与 Task 2 表逐条对应）：

```vue
<script setup lang="ts">
import { computed } from "vue";

const props = defineProps<{ area: string; reason: string; message?: string }>();
const emit = defineEmits<{ retry: []; restartBridge: []; openSettings: [] }>();

type Action = "retry" | "restart" | "settings";
interface Entry { text: string; actions: Action[]; }

const dictionary: Record<string, Entry> = {
  "blender-not-ready": { text: "Blender 链路未就绪：请打开 Blender 并启动 MCP 插件", actions: ["retry", "restart"] },
  "unity-not-ready": { text: "Unity 链路未就绪：请导入工程并确认 MCP 包运行", actions: ["retry", "restart"] },
  "agent-not-configured": { text: "Agent 模型未配置：设置 → Agent 模型", actions: ["settings"] },
  "cloud3d-not-configured": { text: "3D 服务未配置：设置 → 3D 模型", actions: ["settings"] },
  "invalid-project": { text: "不是有效 Unity 工程（需含 Assets 与 ProjectSettings）", actions: [] },
  "cloud3d-failed": { text: "云 3D 任务失败（附服务端 message）", actions: ["retry"] },
  "user-cancelled": { text: "已取消", actions: [] },
  "bridge-crashed": { text: "MCP 桥接异常退出，正在自动重连", actions: ["restart"] },
  "project-not-active": { text: "未激活 Unity 工程：请先在顶部导入工程", actions: [] },
  "invalid-attachment": { text: "附件无效：文件不存在或不在资产库内", actions: [] },
  "blender-convert-failed": { text: "glb→fbx 转换失败：请查看 Blender 日志后重试", actions: ["retry", "restart"] },
  "import-failed": { text: "导入 Unity 失败：请查看 Unity Console 后重试", actions: ["retry"] },
  "max-steps-exceeded": { text: "Agent 步数超限，任务已停止", actions: ["retry"] },
  "empty-plan": { text: "建造计划为空（规划结果无分组）：请重试或补充需求描述", actions: ["retry"] },
  "bridge-not-registered": { text: "当前区未注册桥接，无法重启（P6 未接入或配置错误）", actions: [] },
};

const entry = computed(() => dictionary[props.reason]);
const text = computed(() => entry.value?.text ?? (props.message || props.reason));
const actions = computed<Action[]>(() => entry.value?.actions ?? ["retry"]);
</script>

<template>
  <div class="error-card">
    <div class="error-text">{{ text }}</div>
    <div v-if="message && entry" class="error-detail">{{ message }}</div>
    <div class="error-actions">
      <button v-if="actions.includes('retry')" @click="emit('retry')">重试</button>
      <button v-if="actions.includes('restart')" @click="emit('restartBridge')">重启桥接</button>
      <button v-if="actions.includes('settings')" @click="emit('openSettings')">打开设置</button>
    </div>
  </div>
</template>
```

`ChatPanel.vue` 的 `error.card` 处理改为追加错误项，并接三类动作：

```ts
interface ErrorItem { kind: "error"; area: string; reason: string; message?: string; }

function pushError(d: any) {
  messages.value.push({ kind: "error", area: d.area ?? props.area,
                        reason: d.reason, message: d.message });
}

async function retryError(item: ErrorItem) {
  const res = (await props.client.request("chat.retry", { area: item.area })) as any;
  if (res?.accepted === false)
    pushError({ ...item, reason: res.error ?? item.reason });
}

async function restartBridge(item: ErrorItem) {
  const res = (await props.client.request("bridge.restart", { area: item.area })) as any;
  if (!res?.ok)
    pushError({ kind: "error", area: item.area, reason: "bridge-not-registered" });
}
```

模板中把 P3 的 `role === "error"` 红卡替换为组件渲染（`ChatItem` 类型同步追加 `kind?: "error"`）：

```vue
<ErrorCard v-else-if="m.kind === 'error'" :area="m.area" :reason="m.reason" :message="m.message"
           @retry="retryError(m)" @restart-bridge="restartBridge(m)"
           @open-settings="emit('openSettings')" />
```

> `ChatPanel` 新增 `defineEmits<{ openSettings: [] }>()`，`ErrorCard` 的 `openSettings` 直接透传给 `App.vue` 打开设置弹窗；错误项不被 `chat.clear` 之外的事件清除。

`TopBar.vue`（P3 版本上扩展 hints 与日志入口）：

```vue
<script setup lang="ts">
const props = defineProps<{
  area: string;
  blender: string;
  unity: string;
  hints?: { blender?: string; unity?: string };
  importing?: boolean;
}>();
const emit = defineEmits<{ importProject: []; openLogs: [] }>();

const color = (s: string) =>
  s === "ready" ? "#4ec9b0" : s === "reconnecting" ? "#cca700" : "#555";
const lampTitle = (key: "blender" | "unity") =>
  props.hints?.[key] ??
  (props[key] === "ready" ? "已就绪" : props[key] === "reconnecting" ? "重连中" : "未连接");
</script>

<template>
  <header class="topbar">
    <span v-if="area === 'blender'" class="fixed-path">模型库：文档/SynthForgeStudio/ModelLibrary（固定）</span>
    <button v-else class="import" :disabled="importing" @click="emit('importProject')">导入 Unity 工程</button>
    <span class="spacer" />
    <span class="lamp" :title="lampTitle('blender')"><i :style="{ background: color(blender) }" />Blender</span>
    <span class="lamp" :title="lampTitle('unity')"><i :style="{ background: color(unity) }" />Unity</span>
    <button class="logs" @click="emit('openLogs')">日志</button>
  </header>
</template>
```

`LogDrawer.vue`（右上角抽屉：环境自检 + 最近 200 条 warning/error）：

```vue
<script setup lang="ts">
import { ref } from "vue";

interface LogEntry { level: "warning" | "error"; category: string; message: string; time: string; }
interface EnvItem { state?: string; detail?: string; }
const props = defineProps<{
  open: boolean;
  env?: { uvx?: EnvItem; blender?: EnvItem; unity?: EnvItem };
}>();
const emit = defineEmits<{ close: [] }>();

const entries = ref<LogEntry[]>([]);
function push(entry: LogEntry) {
  entries.value.unshift(entry);
  if (entries.value.length > 200)
    entries.value.length = 200;
}
defineExpose({ push });

const envText = (item?: EnvItem) =>
  !item ? "未检查" : item.state === "ready" ? (item.detail || "就绪") : (item.detail || "失败");
</script>

<template>
  <div v-if="open" class="log-mask" @click.self="emit('close')">
    <aside class="log-drawer">
      <header><span>诊断</span><button @click="emit('close')">✕</button></header>
      <section class="env">
        <div>uvx：{{ envText(env?.uvx) }}</div>
        <div>Blender：{{ envText(env?.blender) }}</div>
        <div>Unity：{{ envText(env?.unity) }}</div>
      </section>
      <section class="logs">
        <div v-if="!entries.length" class="log-empty">暂无警告或错误日志</div>
        <div v-for="(e, i) in entries" :key="i" class="log-line" :class="e.level">
          <span class="log-time">{{ e.time }}</span>
          <span class="log-cat">{{ e.category }}</span>
          <span class="log-msg">{{ e.message }}</span>
        </div>
      </section>
    </aside>
  </div>
</template>
```

`App.vue` 接线（在 P3 版本上追加）：

```ts
const hints = ref<{ blender?: string; unity?: string }>({});
const logsOpen = ref(false);
const logDrawer = ref<InstanceType<typeof LogDrawer>>();
const envReport = ref<{ uvx?: any; blender?: any; unity?: any }>();

client.on("conn.status", (d: any) => {
  lamp.value = { blender: d.blender, unity: d.unity };
  hints.value = d.hints ?? {};
});
client.on("env.status", (d: any) => { envReport.value = d; });
client.on("log.append", (d: any) => logDrawer.value?.push(d));
```

```vue
<TopBar :area="active" :blender="lamp.blender as any" :unity="lamp.unity as any"
        :hints="hints" :importing="picking" @import-project="importProject"
        @open-logs="logsOpen = true" />
<ChatPanel :area="active" :client="client" @open-settings="settingsOpen = true" />
<LogDrawer ref="logDrawer" :open="logsOpen" :env="envReport" @close="logsOpen = false" />
```

> `reconnecting` 的源头是 P1 Task 6 `McpClient::reconnecting(attempt)`；P4 Task 0 的 `McpRuntime` 将其映射为 `statusChanged(area,"reconnecting","第 N 次重连")`，再由 P4 Task 1 的 `CoreApp::broadcastConnStatus` 汇总为 `conn.status {blender, unity, hints}`。P7 只在 TopBar 消费 `hints`，**不重复广播**。

- [ ] **Step 4: CMake + 测试 + Commit**

```cmake
add_executable(tst_env_checker tests/tst_env_checker.cpp)
target_link_libraries(tst_env_checker PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_env_checker COMMAND tst_env_checker)
```

Run: `ctest --test-dir build -C Debug -R tst_env_checker --output-on-failure`
Expected: 1 用例 Passed。Commit（`feat: 环境自检、双灯细化与错误卡字典`）。

---

### Task 3: Release 打包脚本 + 整机冒烟

**Files:** Create `scripts/package.ps1`；Modify `CMakeLists.txt`（install 规则）。

**Interfaces:**
- `scripts/package.ps1`：参数 `-QtKit <path>`；流程：前端构建 → CMake Release 构建 → `windeployqt` → 拷贝 `vcruntime`（VS 已带）→ 产出 `dist/SynthForgeStudio/`。
- `dist/` 内容：`synthforge_app.exe`、Qt 运行库（含 `Qt6WebEngineCore.dll`、`QtWebEngineProcess.exe`、`resources/`、`translations/qtwebengine_locales/`）、`web/dist` 资源已编入 qrc（无需外置）。

- [ ] **Step 1: 写打包脚本**

```powershell
param(
  [Parameter(Mandatory = $true)][string]$QtKit,   # 例：C:\Qt\6.8.3\msvc2022_64
  [string]$Config = "Release"
)
$ErrorActionPreference = 'Stop'
Set-Location (git rev-parse --show-toplevel)

Push-Location web
npm ci
npm run build
Pop-Location

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$QtKit"
cmake --build build --config $Config --target synthforge_app

$stage = "dist/SynthForgeStudio"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item "build/$Config/synthforge_app.exe" $stage

& "$QtKit\bin\windeployqt.exe" --release --no-translations `
    "$stage/synthforge_app.exe"

Write-Host "打包完成：$stage"
```

> 说明：`windeployqt` 会自动收集 WebEngine 依赖（`QtWebEngineProcess`、resources、locales）；若目标机缺 VC 运行库，安装 VC++ 2019-2022 x64 可再发行包（打包说明中注明，不内置）。

- [ ] **Step 2: install 规则（可选冗余）**

```cmake
install(TARGETS synthforge_app RUNTIME DESTINATION .)
```

- [ ] **Step 3: 整机冒烟（干净环境）**

在一台未装 Qt 的 Win10/11 x64 机器（或临时虚拟机）执行：

1. 复制 `dist/SynthForgeStudio/` 整个目录双击 exe → 主窗口出现、无缺 DLL 报错。
2. 设置页保存 Agent 配置 → 重启应用配置仍在（`%APPDATA%/SynthForgeStudio/settings.json`）。
3. 打开 Blender + 插件 → 双灯变绿（≤10s）；关闭 Blender → 灯变黄（重连中）→ 再开 → 恢复绿。
4. 低模全链跑一次（发送 → 草稿 → 选目录 → 生成 → 预览）。
5. 检查 `logs/` 有当天日志且无 ERROR 级噪音；点顶栏「日志」能看到 `env.status` 三链路结果。
6. 关掉 Blender 后发一条消息 → 错误卡出现 →点「重试」重新提交；再点「重启桥接」→ 灯先黄后绿。

- [ ] **Step 4: Commit**（`chore: Release 打包脚本与整机冒烟清单`）

---

## P7 完成标准

- [ ] 日志按天落盘、50MB 轮转、跨天序号归零、stderr 同步输出
- [ ] `log.append` 仅广播 warning/error，日志抽屉环形缓冲≤200 条且 sink 内日志不递归
- [ ] `env.check` 立即返回，`env.status` 异步给出三链路状态；双灯灰/黄/绿与 hints 文案正确
- [ ] 全部 15 个 `error.card.reason` 有中文文案与正确动作；未收录 reason 不出现空白卡
- [ ] `[重试]` 发 `chat.retry {area}`、`[重启桥接]` 发 `bridge.restart {area}`、`[打开设置]` 打开设置弹窗
- [ ] 干净 Windows 机器直接运行 `dist/SynthForgeStudio/synthforge_app.exe`
- [ ] 冒烟清单 6 项全过

---

## 全阶段收尾（P1~P7 汇总验收）

- [ ] `ctest --test-dir build -C Debug` 全部测试可执行全绿（P1 6 个 + P2 5 个 + P4 4 个 + P5 2 个 + P6 3 个 + P7 1 个 = 21 个）
- [ ] 双模式建模真实跑通（低模 Blender MCP / 云 API 至少一家）
- [ ] Unity 区互斥切换 + 文件双源同步 + 添加资产真实跑通
- [ ] 打包产物在干净机器可启动、可用
