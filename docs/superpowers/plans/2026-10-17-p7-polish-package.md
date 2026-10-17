# SynthForge Studio P7：打磨与打包实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 收尾打磨：统一日志（按天分文件）、启动环境自检与错误卡字典、双灯状态细化（连接中/重连中/就绪/失败原因）、Release 打包脚本（windeployqt 产出可分发目录）与整机冒烟清单。

**Architecture:** `LogService` 用 `qInstallMessageHandler` 接管全部 Qt 日志写文件并按天轮转；`EnvChecker` 异步检查 uvx 可用性与两条 MCP 链路状态，启动自动运行一次并广播 `conn.status` / `env.check`；前端 `ErrorCard` 统一渲染原因字典；打包为 CMake install + windeployqt 脚本，产物放 `dist/`。

**规格映射：** 9 错误处理、4.2 双灯、8.8 环境自检、7.3 日志、11 阶段 7（打磨打包）。

**依赖：** P2~P6 全部。

## Global Constraints

- 日志目录 `%APPDATA%/SynthForgeStudio/logs/`，文件名 `yyyy-MM-dd.log`；单文件超 50MB 时轮转为 `<date>-1.log` 并继续。
- 错误卡原因字典与 `error.card.reason` 枚举一一对应（见 Task 2 表），新增 reason 必须同步前后端。
- 环境自检不得阻塞启动：并行发起、5 秒单项超时、结果以事件推送。
- 打包产物必须可在**未安装 Qt 的 Windows 10/11 机器**直接运行（WebEngine 全家桶齐全）。

## File Structure

```
src/core/logging/LogService.h/.cpp      ← 文件日志接管 + 轮转
src/core/env/EnvChecker.h/.cpp          ← uvx/Blender/Unity 自检
web/src/components/ErrorCard.vue        ← 错误卡组件 + 原因字典
web/src/components/ConnectionLamps.vue  ← 双灯细化（可并入 TopBar）
scripts/package.ps1                     ← Release 打包脚本
tests/tst_env_checker.cpp
```

---

### Task 1: LogService（按天日志 + 轮转）

**Files:** Create `src/core/logging/LogService.h/.cpp`；Modify `src/main.cpp` 与 `src/cli/main.cpp`（启动即安装）。

**Interfaces:**
- `class LogService { static void install(const QString& logDir); static QString currentLogFile(); static void shutdown(); }`
- 行为：接管 `qInstallMessageHandler`；每行格式 `[HH:mm:ss.zzz] [I|W|E] category: message`；同时写文件与 stderr；达到 50MB 轮转。

- [ ] **Step 1: 实现**

```cpp
namespace {
QFile* g_file = nullptr;
QString g_dir;
QDate g_date;
QString g_path;
int g_rotate = 0;

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QDate today = QDate::currentDate();
    const QString wanted = g_dir + "/" + today.toString("yyyy-MM-dd")
                           + (g_rotate ? QString("-%1").arg(g_rotate) : QString()) + ".log";
    if (!g_file || g_date != today || g_path != wanted) {
        delete g_file;
        g_date = today;
        g_path = wanted;
        QDir().mkpath(g_dir);
        g_file = new QFile(g_path);
        g_file->open(QIODevice::Append | QIODevice::WriteOnly);
    }
    if (g_file->size() > 50 * 1024 * 1024) {          // 轮转
        g_file->close();
        delete g_file;
        ++g_rotate;
        g_file = nullptr;
        return handler(type, context, message);        // 重新按新文件名打开
    }
    const char level = type == QtDebugMsg    ? 'D'
                       : type == QtInfoMsg   ? 'I'
                       : type == QtWarningMsg ? 'W'
                                            : 'E';
    const QString line = QStringLiteral("[%1] [%2] %3: %4\n")
                             .arg(QTime::currentTime().toString("HH:mm:ss.zzz"))
                             .arg(QLatin1Char(level))
                             .arg(QString::fromUtf8(context.category ? context.category : "app"),
                                  message);
    if (g_file)
        g_file->write(line.toUtf8());
    g_file->flush();
    fprintf(stderr, "%s", line.toUtf8().constData());
}
}

void LogService::install(const QString& logDir)
{
    g_dir = logDir;
    qInstallMessageHandler(handler);
}
```

- [ ] **Step 2: 接线**：`main.cpp` / `cli/main.cpp` 开头（设置 QCoreApplication 名称后）调用 `LogService::install(defaultDataDir() + "/logs")`；`qInfo()` 打印版本与启动参数。

- [ ] **Step 3: 验证**：启动 app → 日志文件生成；消息同时出现在文件与 stderr。Commit（`feat: 按天文件日志与轮转`）。

---

### Task 2: EnvChecker + 双灯细化 + 错误卡字典

**Files:** Create `src/core/env/EnvChecker.h/.cpp`、`web/src/components/ErrorCard.vue`；Modify `CoreApp`（启动时 `QTimer::singleShot(0, …)` 异步自检；新增 `env.check` handler、`conn.status` 广播）。

**Interfaces:**
- `struct EnvItem { QString state; /* unknown|checking|ready|failed */ QString detail; }`
- `class EnvChecker : QObject { void run(); signals: void result(QJsonObject /* {uvx, blender, unity} */); }`
  - `uvx`：`QProcess` 执行 `uvx --version`，5s 超时；成功回 `{state:"ready", detail:"uvx x.y.z"}`。
  - `blender` / `unity`：对应 `McpClient::isReady()` + 工具数；未连接回 `{state:"failed", detail:"blender-not-ready"}` 等。
- `conn.status` 事件载荷：`{blender:"off|ready|reconnecting", unity:"...", hints:{blender?:detail, unity?:detail}}`。
- `error.card` 原因字典（前后端同步）：

| reason | 展示文案 |
|---|---|
| `blender-not-ready` | Blender 链路未就绪：请打开 Blender 并启动 MCP 插件 |
| `unity-not-ready` | Unity 链路未就绪：请导入工程并确认 MCP 包运行 |
| `agent-not-configured` | Agent 模型未配置：设置 → Agent 模型 |
| `cloud3d-not-configured` | 3D 服务未配置：设置 → 3D 模型 |
| `invalid-project` | 不是有效 Unity 工程（需含 Assets 与 ProjectSettings） |
| `cloud3d-failed` | 云 3D 任务失败（附服务端 message） |
| `user-cancelled` | 已取消 |
| `bridge-crashed` | MCP 桥接异常退出，正在自动重连 |

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
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QTimer::singleShot(5000, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, report](int code) {
                (*report)["uvx"] = QJsonObject{
                    {"state", code == 0 ? "ready" : "failed"},
                    {"detail", QString::fromUtf8(proc->readAll()).trimmed()}};
                (*report)["blender"] = m_blender && m_blender->isReady()
                                           ? QJsonObject{{"state", "ready"}}
                                           : QJsonObject{{"state", "failed"},
                                                         {"detail", "blender-not-ready"}};
                (*report)["unity"] = m_unity && m_unity->isReady()
                                         ? QJsonObject{{"state", "ready"}}
                                         : QJsonObject{{"state", "failed"},
                                                       {"detail", "unity-not-ready"}};
                emit result(*report);
                proc->deleteLater();
            });
    proc->start(m_uvxPath, {QStringLiteral("--version")});
}
```

- [ ] **Step 3: 前端错误卡与双灯**

`ErrorCard.vue`：props `{reason, message?}`；查字典渲染文案 + 可选「打开设置」跳转（emit `openSettings`）。`ChatPanel.vue` 的 `error.card` 处理改为追加 `ErrorCard` 项。

TopBar 双灯悬停 `title` 使用 `hints` 字段；`reconnecting` 状态由 P2 `McpClient` 重连钩子广播（P7 补充 `McpRuntime::statusChanged` → `conn.status`）。

- [ ] **Step 4: CMake + 测试 + Commit**（`feat: 环境自检、双灯细化与错误卡字典`）

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
5. 检查 `logs/` 有当天日志且无 ERROR 级噪音。

- [ ] **Step 4: Commit**（`chore: Release 打包脚本与整机冒烟清单`）

---

## P7 完成标准

- [ ] 日志按天落盘、50MB 轮转、stderr 同步输出
- [ ] `env.check` 返回三链路状态；双灯灰/黄/绿与 hints 文案正确
- [ ] 全部 8 个 `error.card.reason` 有中文文案且可达（可用 mock 或断链触发）
- [ ] 干净 Windows 机器直接运行 `dist/SynthForgeStudio/synthforge_app.exe`
- [ ] 冒烟清单 5 项全过

---

## 全阶段收尾（P1~P7 汇总验收）

- [ ] `ctest --test-dir build -C Debug` 全部测试可执行全绿（P1 5 个 + P2 5 个 + P4 2 个 + P5 2 个 + P6 3 个 + P7 1 个）
- [ ] 双模式建模真实跑通（低模 Blender MCP / 云 API 至少一家）
- [ ] Unity 区互斥切换 + 文件双源同步 + 添加资产真实跑通
- [ ] 打包产物在干净机器可启动、可用
