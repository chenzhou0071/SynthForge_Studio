# SynthForge Studio P6：Unity 区全链实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unity 区完整闭环：工程导入校验与**全局唯一激活互斥切换**（断旧链 → 取消旧工程任务 → 切换）→ Unity agent 任务（工具裁剪 / Game 截图 / Console 回读）→ 工程目录文件监控（防抖 + 限流 + 双源去重）→「添加资产」选择器（glb→fbx 转换 → unity 导入实例化）。

**Architecture:** `ProjectService` 维护唯一激活工程与切换状态机；`DirectoryWatcher` 封装 QFileSystemWatcher（Windows 底层即 ReadDirectoryChangesW）做递归注册、400ms 防抖、事件限流与全量刷新兜底；`McpOperationWindow` 在 MCP 工具调用前后置抑制窗口，吞掉 watcher 回声事件实现双源去重；Unity agent 复用 P4 `AgentOrchestrator`，仅替换工具集与区名。

**规格映射：** 6.1 工程导入、6.2 激活互斥切换、6.3 Unity agent 流程、6.4 添加资产跨区联动、12 风险 6（watcher 溢出）、8.3 方法/事件族（`dialog.pickFolder` / `dialog.result`，v1 唯一一对扩展）。

**依赖：** P4（Orchestrator / 聊天链路 / 文件树）、P5（GLB 资产）。

## Global Constraints

- 激活互斥：任何时刻至多一个 Unity 工程处于 `active`；切换顺序固定为「旧工程 deactivating（断 MCP + 取消任务）→ 新工程 activating → active」。
- 工程校验：目录必须同时含 `Assets/` 与 `ProjectSettings/`，否则拒绝并 `error.card {reason:"invalid-project"}`。
- Watcher 忽略：路径含 `/Library/`、`/Temp/`、`/obj/`、`/Logs/`，或后缀 `.meta`。
- 防抖 400ms；单批 >200 条变更时降级为「全量刷新」单事件；抑制窗口内事件一律丢弃（MCP 自身通知负责定向更新）。
- Unity 区对话与会话存储与 blender 区完全隔离（`area:"unity"`）。
- 切换确认：旧工程有运行/排队任务时，`project.select` 返回 `{accepted:false, reason:"switch-confirm", currentPath, newPath, runningTasks}`；前端确认后发 `project.switchConfirm`。切换确认本身不新增事件名（确认信息走返回值）。
- 目录选择：`dialog.pickFolder {purpose:"unityProject", requestId}` → C++ 唤起系统文件夹选择框；选中/取消统一广播 `dialog.result {requestId, path?, cancelled}`。C++ 只回传路径，校验与切换仍走 `project.select`；取消不产生 `error.card`。
- 等待 Unity：激活后若 MCP 未就绪，进入「等待 Unity 启动」，5s 间隔最多 12 次（60s）；超时 `error.card {reason:"unity-not-ready"}`，工程保持 `activating` 以便再次 `project.select` 重试。
- 记住上次工程：路径存 `settings.json` 的 `unity.lastProject`；启动时若目录仍合法则直接 `select` 恢复（不弹确认）。
- 记住上次浏览目录：选中后写 `unity.lastBrowseDir`，下次打开对话框从该目录开始；无记录时用上次工程的父目录，再退到「文档」。
- Unity 区路径基准：树、`file.read`、`fs.operate` 的相对路径均以 `<project>/Assets` 为根；`tree.refresh {area:"unity", path?}` 为唯一树数据源。
- 附件 chip：unity 区 `chat.send` 支持 `attachments: [相对资产库路径]`；发送后前端清空 chip；`.glb` 无配对 `.fbx` 时先经 Blender 链转换。
- **并发与门禁**：ProjectService 只负责 Unity 工程激活，不感知 Blender 区任务；两区普通任务（blender 低模 / 修改 vs unity agent）可并行。仅 Unity 的 GLB→FBX 转换与 Blender 任务共用同一把 `BlenderGate`（P4 Task 2B），忙时返回 `blender-busy`，不自动排队。
- Unity 区文件事件只由 watcher 产生（`fileTree.changed {area:"unity", reason, paths}`）；MCP 工具调用期间由抑制窗口吞掉回声。

## File Structure

```
src/core/unity/ProjectService.h/.cpp    ← 校验 + 激活互斥状态机 + 等待 Unity + 工程根解析
src/core/unity/DirectoryWatcher.h/.cpp  ← 递归监控 + 防抖 + 限流 + 抑制窗口
src/core/unity/AssetImporter.h/.cpp     ← glb→fbx 转换 + 附件准备 + unity 导入
web/src/components/AssetPicker.vue      ← 添加资产选择器（模型文件多选）
web/src/components/FileTree.vue         ← Modify：unity 树（tree.refresh + 局部刷新）
tests/tst_project_service.cpp
tests/tst_folder_pick.cpp
tests/tst_watcher.cpp
tests/tst_asset_importer.cpp
```

---

### Task 1: ProjectService + 原生目录选择（校验/切换/等待 + dialog.pickFolder）

**Files:** Create `src/core/unity/ProjectService.h/.cpp`、`tests/tst_project_service.cpp`、`tests/tst_folder_pick.cpp`；Modify `CoreApp`（`project.select` / `project.switchConfirm` / `dialog.pickFolder` handler、`project.state` 广播、上次工程持久化）、`src/main.cpp`（注入 GUI 文件夹选择器）。

**Interfaces:**
- `struct ProjectInfo { QString path; QString name; QString state; int attempt = 0; }`（`state ∈ inactive|deactivating|activating|active`）；头文件末尾 `Q_DECLARE_METATYPE(ProjectInfo)`，构造函数内 `qRegisterMetaType<ProjectInfo>()`。
- `enum class SwitchResult { Started, NeedsConfirm, Invalid, Same };`
- `class ProjectService : QObject`：
  - `bool validate(const QString& path) const`（`Assets/` + `ProjectSettings/` 同时存在）。
  - `SwitchResult requestSwitch(const QString& path, int* runningTasks = nullptr)`：校验 → 同路径返回 `Same` → 旧工程运行/排队任务数 > 0 返回 `NeedsConfirm`（不切换）→ 否则 `select(path)` 并返回 `Started`。
  - `void confirmSwitch(const QString& path)`（`project.switchConfirm` 确认后调用，内部 `select`）。
  - `void select(const QString& path)`（直接切换；启动恢复上次工程也用它）。
  - `QString activePath() const`；`QString assetsPath() const`（`<active>/Assets`）；`QString absoluteFor(const QString& relative) const`；`bool isInsideAssets(const QString& absolute) const`。
  - 注入：`setBridgeHooks(stopBridge, startBridge, cancelProjectTasks)`、`setBridgeReadyQuery(std::function<bool()>)`、`setRunningTaskQuery(std::function<int()>)`、`setWaitIntervalForTest(int ms)`。
  - 信号：`stateChanged(ProjectInfo)`、`waitingUnity(path, attempt)`、`errorOccurred(reason)`。**不新增 `switchRequired` 信号**——确认信息走 `project.select` 的返回值。
  - 并发边界：ProjectService 不感知 Blender 区任务；Blender 会话互斥由 CoreApp 的 `BlenderGate` 统一协调（P4 Task 2B），P6 只在 AssetImporter 转换期间使用它。
- `dialog.pickFolder {purpose:"unityProject", requestId}` → 立即返回 `{accepted:true}`；拒绝返回 `{accepted:false, reason}`（`busy` / `unsupported-purpose` / `bad-request` / `dialog-unavailable`）。用户选中或取消后广播 `dialog.result {requestId, path?, cancelled}`；`requestId` 由前端生成（避免事件先于响应到达时丢结果）。
- `CoreApp::setFolderPicker(std::function<void(const QString& startDir, std::function<void(const QString&)> done)>)` — 由 GUI 入口注入（`QFileDialog`）；core 不依赖 QtWidgets。未注入（headless/单测）→ `dialog-unavailable`；单测注入 fake 直接回调临时目录。
- Consumes: `TaskManager::runningCount(area)`（P2 Task 3 追加，含运行中 + 排队数）、`SettingsStore`（P2 Task 1）。

- [x] **Step 1: 写失败测试**

```cpp
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include "core/unity/ProjectService.h"

class ProjectServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void validateRequiresAssetsAndProjectSettings()
    {
        QTemporaryDir dir;
        ProjectService service;
        QVERIFY(!service.validate(dir.path()));          // 空目录不合法
        QDir(dir.path()).mkdir("Assets");
        QDir(dir.path()).mkdir("ProjectSettings");
        QVERIFY(service.validate(dir.path()));
    }

    void switchOrderIsDeactivateThenActivate()
    {
        QTemporaryDir dirA, dirB;
        for (auto* d : {&dirA, &dirB}) {
            QDir(d->path()).mkdir("Assets");
            QDir(d->path()).mkdir("ProjectSettings");
        }
        ProjectService service;
        QStringList events;
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) {
                             events << info.path + ":" + info.state;
                         });
        service.setBridgeHooks([] { /* stop */ }, [] { /* start */ },
                               [&](const QString&) { /* cancel tasks */ });

        service.select(dirA.path());
        service.select(dirB.path());

        QCOMPARE(events.size(), 5);
        QCOMPARE(events.at(0), dirA.path() + ":activating");
        QCOMPARE(events.at(1), dirA.path() + ":active");
        QCOMPARE(events.at(2), dirA.path() + ":deactivating");
        QCOMPARE(events.at(3), dirB.path() + ":activating");
        QCOMPARE(events.at(4), dirB.path() + ":active");
    }

    void switchWithRunningTasksRequiresConfirmation()
    {
        QTemporaryDir dirA, dirB;
        for (auto* d : {&dirA, &dirB}) {
            QDir(d->path()).mkdir("Assets");
            QDir(d->path()).mkdir("ProjectSettings");
        }
        ProjectService service;
        service.setBridgeHooks([] {}, [] {}, [](const QString&) {});
        service.setRunningTaskQuery([] { return 2; });
        QStringList states;
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) { states << info.state; });

        service.select(dirA.path());
        int running = 0;
        QCOMPARE(service.requestSwitch(dirB.path(), &running),
                 ProjectService::SwitchResult::NeedsConfirm);
        QCOMPARE(running, 2);
        QCOMPARE(states.size(), 2);                     // 仍停在 A（activating/active）
        QCOMPARE(service.activePath(), dirA.path());

        service.confirmSwitch(dirB.path());
        QCOMPARE(states.size(), 5);
        QCOMPARE(service.activePath(), dirB.path());
    }

    void waitsForUnityThenActivates()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("Assets");
        QDir(dir.path()).mkdir("ProjectSettings");
        ProjectService service;
        bool ready = false;
        service.setBridgeHooks([] {}, [] {}, [](const QString&) {});
        service.setBridgeReadyQuery([&] { return ready; });
        service.setWaitIntervalForTest(10);             // 生产 5000ms × 12 次

        QStringList states;
        QSignalSpy waiting(&service, &ProjectService::waitingUnity);
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) { states << info.state; });

        service.select(dir.path());
        QCOMPARE(states, QStringList{QStringLiteral("activating")});
        QTRY_VERIFY(waiting.count() >= 2);              // 未就绪：进入等待并自动重试
        ready = true;
        QTRY_COMPARE(states.size(), 2);
        QCOMPARE(states.last(), QStringLiteral("active"));
    }
};

QTEST_MAIN(ProjectServiceTest)
#include "tst_project_service.moc"
```

- [x] **Step 2: 实现**

```cpp
ProjectService::ProjectService(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ProjectInfo>();
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, &ProjectService::tryActivate);
}

bool ProjectService::validate(const QString& path) const
{
    const QDir dir(path);
    return dir.exists(QStringLiteral("Assets")) && dir.exists(QStringLiteral("ProjectSettings"));
}

ProjectService::SwitchResult ProjectService::requestSwitch(const QString& path, int* running)
{
    if (!validate(path))
        return SwitchResult::Invalid;
    if (path == m_active)
        return SwitchResult::Same;
    const int count = m_hooks.runningTasks ? m_hooks.runningTasks() : 0;
    if (running)
        *running = count;
    if (!m_active.isEmpty() && count > 0)
        return SwitchResult::NeedsConfirm;              // 前端弹确认框，不切换
    select(path);
    return SwitchResult::Started;
}

void ProjectService::confirmSwitch(const QString& path)
{
    select(path);
}

void ProjectService::select(const QString& path)
{
    if (!validate(path)) {
        emit errorOccurred(QStringLiteral("invalid-project"));
        return;
    }
    if (path == m_active)
        return;

    m_waitTimer.stop();
    m_waitAttempt = 0;

    if (!m_active.isEmpty()) {
        emit stateChanged({m_active, QFileInfo(m_active).fileName(),
                           QStringLiteral("deactivating"), 0});
        m_hooks.cancelProjectTasks(m_active);           // 终止该工程绑定的 Agent 任务
        m_hooks.stopBridge();                           // 断开旧 MCP 会话
    }

    emit stateChanged({path, QFileInfo(path).fileName(),
                       QStringLiteral("activating"), 0});
    m_active = path;
    m_hooks.startBridge();                              // 拉起新 MCP 会话
    tryActivate();
}

void ProjectService::tryActivate()
{
    if (m_active.isEmpty())
        return;
    if (!m_hooks.isReady || m_hooks.isReady()) {        // 未注入视为已就绪（单测路径）
        m_waitTimer.stop();
        emit stateChanged({m_active, QFileInfo(m_active).fileName(),
                           QStringLiteral("active"), 0});
        return;
    }
    if (++m_waitAttempt > m_maxAttempts) {              // 5s × 12 = 60s
        emit errorOccurred(QStringLiteral("unity-not-ready"));
        return;                                         // 保持 activating，可再次 project.select
    }
    emit waitingUnity(m_active, m_waitAttempt);
    m_waitTimer.start(m_waitIntervalMs);
}

QString ProjectService::assetsPath() const
{
    return m_active.isEmpty() ? QString() : m_active + QStringLiteral("/Assets");
}

QString ProjectService::absoluteFor(const QString& relative) const
{
    return QDir(assetsPath()).filePath(relative);
}

bool ProjectService::isInsideAssets(const QString& absolute) const
{
    if (m_active.isEmpty())
        return false;
    const QString root = QFileInfo(assetsPath()).canonicalFilePath();
    QString path = QFileInfo(absolute).canonicalFilePath();
    if (path.isEmpty()) {                               // 目标尚不存在：校验父目录
        const QFileInfo info(absolute);
        path = QDir::cleanPath(info.absoluteDir().canonicalPath() + "/" + info.fileName());
    }
    return !path.isEmpty() && (path == root || path.startsWith(root + '/'));
}
```

`CoreApp` 接线（`start()` 中）：

```cpp
m_projects = new ProjectService(this);
m_projects->setBridgeHooks(
    [] {}, [] {},                                   // Task 3 替换为 m_unity->stop()/start()
    [this](const QString&) { m_tasks->cancelArea("unity"); });
m_projects->setBridgeReadyQuery([] { return true; });  // Task 3 替换为 m_unity->isReady()
m_projects->setRunningTaskQuery([this] { return m_tasks->runningCount("unity"); });

connect(m_projects, &ProjectService::stateChanged, this, [this](const ProjectInfo& info) {
    if (info.state == QStringLiteral("active")) {   // 记住上次工程（规格 6.1）
        QJsonObject root = m_settings->data();
        QJsonObject unity = root.value(QStringLiteral("unity")).toObject();
        unity.insert(QStringLiteral("lastProject"), info.path);
        root.insert(QStringLiteral("unity"), unity);
        m_settings->setData(root);
        m_settings->save();
    }
    m_ws->broadcastEvent("project.state",
                         {{"path", info.path}, {"name", info.name}, {"state", info.state}});
});
connect(m_projects, &ProjectService::waitingUnity, this,
        [this](const QString& path, int attempt) {
            m_ws->broadcastEvent("project.state",
                                 {{"path", path}, {"state", "activating"},
                                  {"waiting", true}, {"attempt", attempt}});
        });
connect(m_projects, &ProjectService::errorOccurred, this, [this](const QString& reason) {
    m_ws->broadcastEvent("error.card", {{"area", "unity"}, {"reason", reason}});
});

m_router->registerHandler("project.select", [this](const QJsonObject& params) {
    int running = 0;
    const auto result = m_projects->requestSwitch(params.value("path").toString(), &running);
    if (result == ProjectService::SwitchResult::Invalid)
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "invalid-project"}});
    if (result == ProjectService::SwitchResult::NeedsConfirm)
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "switch-confirm"},
                                      {"currentPath", m_projects->activePath()},
                                      {"newPath", params.value("path").toString()},
                                      {"runningTasks", running}});
    return QJsonValue(QJsonObject{{"accepted", true}});
});
m_router->registerHandler("project.switchConfirm", [this](const QJsonObject& params) {
    m_projects->confirmSwitch(params.value("path").toString());
    return QJsonValue(QJsonObject{{"accepted", true}});
});
```

- [x] **Step 3: 写失败的 `tst_folder_pick`（4 用例）**

```cpp
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QWebSocket>
#include <QtTest>
#include <functional>
#include "core/coreapp/CoreApp.h"

class FolderPickTest : public QObject
{
    Q_OBJECT

    static QJsonObject waitFor(QSignalSpy& frames,
                               const std::function<bool(const QJsonObject&)>& match,
                               int timeoutMs = 3000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            for (const QList<QVariant>& args : frames) {
                const QJsonObject obj =
                    QJsonDocument::fromJson(args.at(0).toString().toUtf8()).object();
                if (match(obj))
                    return obj;
            }
            QTest::qWait(20);
        }
        return {};
    }

    static QWebSocket* connectTo(CoreApp& app)
    {
        auto* socket = new QWebSocket;
        QSignalSpy connected(socket, &QWebSocket::connected);
        socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                              .arg(app.ws()->port()).arg(app.ws()->token())));
        if (!connected.wait(5000)) {              // 事件循环内等待，避免阻塞服务端 accept
            delete socket;
            return nullptr;
        }
        return socket;
    }

private slots:
    void selectedPathIsBroadcast()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        std::function<void(const QString&)> finish;
        QString startDir;
        app.setFolderPicker([&](const QString& start, std::function<void(const QString&)> done) {
            startDir = start;
            finish = std::move(done);
        });
        QVERIFY(app.start());

        QWebSocket* socket = connectTo(app);
        QVERIFY(socket);
        QSignalSpy frames(socket, &QWebSocket::textMessageReceived);
        socket->sendTextMessage(R"({"id":1,"method":"dialog.pickFolder",
            "params":{"purpose":"unityProject","requestId":"r1"}})");
        QTRY_VERIFY(finish != nullptr);
        QVERIFY(!startDir.isEmpty());                      // 默认目录：上次浏览/工程父目录/Documents
        const QJsonObject accepted = waitFor(frames, [](const QJsonObject& o) {
            return o.value("id").toInt() == 1;
        });
        QCOMPARE(accepted.value("result").toObject().value("accepted").toBool(), true);

        finish(dir.path() + "/MyGame");                   // 模拟用户选中
        const QJsonObject event = waitFor(frames, [](const QJsonObject& o) {
            return o.value("event").toString() == QStringLiteral("dialog.result");
        });
        const QJsonObject data = event.value("data").toObject();
        QCOMPARE(data.value("requestId").toString(), QStringLiteral("r1"));
        QCOMPARE(data.value("cancelled").toBool(), false);
        QCOMPARE(data.value("path").toString(), QDir::cleanPath(dir.path() + "/MyGame"));
        delete socket;
    }

    void cancelledIsSilent()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        std::function<void(const QString&)> finish;
        app.setFolderPicker([&](const QString&, std::function<void(const QString&)> done) {
            finish = std::move(done);
        });
        QVERIFY(app.start());
        QWebSocket* socket = connectTo(app);
        QVERIFY(socket);
        QSignalSpy frames(socket, &QWebSocket::textMessageReceived);
        socket->sendTextMessage(R"({"id":2,"method":"dialog.pickFolder",
            "params":{"purpose":"unityProject","requestId":"r2"}})");
        QTRY_VERIFY(finish != nullptr);
        finish(QString());                                 // 用户取消
        const QJsonObject event = waitFor(frames, [](const QJsonObject& o) {
            return o.value("event").toString() == QStringLiteral("dialog.result");
        });
        const QJsonObject data = event.value("data").toObject();
        QCOMPARE(data.value("requestId").toString(), QStringLiteral("r2"));
        QCOMPARE(data.value("cancelled").toBool(), true);
        QVERIFY(data.value("path").toString().isEmpty());
        delete socket;
    }

    void secondRequestWhileOpenIsBusy()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        std::function<void(const QString&)> finish;
        app.setFolderPicker([&](const QString&, std::function<void(const QString&)> done) {
            finish = std::move(done);
        });
        QVERIFY(app.start());
        QWebSocket* socket = connectTo(app);
        QVERIFY(socket);
        QSignalSpy frames(socket, &QWebSocket::textMessageReceived);
        socket->sendTextMessage(R"({"id":3,"method":"dialog.pickFolder",
            "params":{"purpose":"unityProject","requestId":"r3"}})");
        QTRY_VERIFY(finish != nullptr);
        socket->sendTextMessage(R"({"id":4,"method":"dialog.pickFolder",
            "params":{"purpose":"unityProject","requestId":"r4"}})");
        const QJsonObject busy = waitFor(frames, [](const QJsonObject& o) {
            return o.value("id").toInt() == 4;
        });
        const QJsonObject result = busy.value("result").toObject();
        QCOMPARE(result.value("accepted").toBool(), false);
        QCOMPARE(result.value("reason").toString(), QStringLiteral("busy"));
        finish(QString());                                 // 收尾：关掉第一个请求
        delete socket;
    }

    void unavailableWithoutPicker()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});                    // 未注入 picker（headless）
        QVERIFY(app.start());
        QWebSocket* socket = connectTo(app);
        QVERIFY(socket);
        QSignalSpy frames(socket, &QWebSocket::textMessageReceived);
        socket->sendTextMessage(R"({"id":5,"method":"dialog.pickFolder",
            "params":{"purpose":"unityProject","requestId":"r5"}})");
        const QJsonObject frame = waitFor(frames, [](const QJsonObject& o) {
            return o.value("id").toInt() == 5;
        });
        const QJsonObject result = frame.value("result").toObject();
        QCOMPARE(result.value("accepted").toBool(), false);
        QCOMPARE(result.value("reason").toString(), QStringLiteral("dialog-unavailable"));
        delete socket;
    }
};

QTEST_MAIN(FolderPickTest)
#include "tst_folder_pick.moc"
```

Run: `cmake --build build --config Debug --target tst_folder_pick`
Expected: 编译失败（`CoreApp::setFolderPicker` 尚未定义）。

- [x] **Step 4: 实现 `dialog.pickFolder` + GUI 注入**

`CoreApp.h`：

```cpp
using FolderPicker = std::function<void(const QString& startDir,
                                        std::function<void(const QString&)> done)>;
void setFolderPicker(FolderPicker picker) { m_folderPicker = std::move(picker); }
// ...
FolderPicker m_folderPicker;
bool m_pickingFolder = false;
```

`CoreApp::registerHandlers()`（`project.switchConfirm` 之后）：

```cpp
m_router->registerHandler("dialog.pickFolder", [this](const QJsonObject& params) {
    if (!m_folderPicker)
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "dialog-unavailable"}});
    if (m_pickingFolder)
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "busy"}});
    if (params.value("purpose").toString() != QLatin1String("unityProject"))
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "unsupported-purpose"}});
    const QString requestId = params.value("requestId").toString();
    if (requestId.isEmpty())
        return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "bad-request"}});

    const QJsonObject unity = m_settings->data().value("unity").toObject();
    QString startDir = unity.value("lastBrowseDir").toString();
    if (startDir.isEmpty()) {
        const QString lastProject = unity.value("lastProject").toString();
        if (!lastProject.isEmpty())
            startDir = QFileInfo(lastProject).absolutePath();
    }
    if (startDir.isEmpty())
        startDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    m_pickingFolder = true;
    m_folderPicker(startDir, [this, requestId](const QString& path) {
        m_pickingFolder = false;
        const QString clean = path.isEmpty() ? QString() : QDir::cleanPath(path);
        if (!clean.isEmpty()) {                            // 记住上次浏览目录
            QJsonObject root = m_settings->data();
            QJsonObject unity = root.value("unity").toObject();
            unity.insert("lastBrowseDir", clean);
            root.insert("unity", unity);
            m_settings->setData(root);
            m_settings->save();
        }
        m_ws->broadcastEvent("dialog.result",
                             {{"requestId", requestId},
                              {"path", clean},
                              {"cancelled", clean.isEmpty()}}});
    });
    return QJsonValue(QJsonObject{{"accepted", true}});
});
```

> `CoreApp.h` 需 `#include <functional>`、`<utility>`（`std::move`）；`CoreApp.cpp` 需 `#include <QFileInfo>`、`<QStandardPaths>`。`setFolderPicker` 只由 GUI 入口调用，`synthforge_core` 不链接 QtWidgets；单测注入 fake，headless 不注入。

`src/main.cpp`（`MainWindow window(uiUrl);` 之后、`window.show()` 之前；`core` 必须在 `window` 之前声明，确保退出时先毁窗口再毁内核）：

```cpp
#include <QFileDialog>

core.setFolderPicker([&window](const QString& startDir,
                               std::function<void(const QString&)> done) {
    auto* dialog = new QFileDialog(&window, QStringLiteral("选择 Unity 工程文件夹"), startDir);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setWindowModality(Qt::ApplicationModal);     // 非 exec()，但保持应用级模态
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::accepted, dialog, [dialog, done] {
        done(dialog->selectedFiles().value(0));
    });
    QObject::connect(dialog, &QDialog::rejected, dialog, [done] { done(QString()); });
    dialog->open();                                      // 不阻塞事件循环
});
```

Run: `cmake --build build --config Debug --target tst_folder_pick; ctest --test-dir build -C Debug -R tst_folder_pick --output-on-failure`
Expected: 4 用例 Passed。

- [x] **Step 5: 前端导入改为原生选择**

前端 TopBar「导入」→ `dialog.pickFolder`（系统文件夹选择框）→ `project.select`；返回 `reason:"switch-confirm"` 时弹确认框（文案「当前工程有 N 个任务运行中，切换将终止这些任务」），确认后发 `project.switchConfirm {path}`。`project.state` 的 `waiting:true` 显示为「等待 Unity 启动（第 N 次尝试）」。

> **原生目录选择**：WebEngine 的 `<input webkitdirectory>` 只能拿到相对路径，因此 v1 明确新增一对方法/事件 `dialog.pickFolder` + `dialog.result`（规格 8.3 已登记，是唯一一对扩展）。C++ 只负责唤起对话框并回传路径，校验/切换仍全部走 `project.select`。

`App.vue` 导入与等待提示（P7 的 TopBar 只改灯/日志，不覆盖这段）：

```ts
const importError = ref("");
const picking = ref(false);
const projectState = ref<any>(null);

// 先订阅再发请求：requestId 由前端生成，事件即使早于响应到达也不会丢
function waitDialogResult(requestId: string) {
  let off = () => {};
  let timer: ReturnType<typeof setTimeout>;
  const promise = new Promise<string | null>((resolve, reject) => {
    timer = setTimeout(() => { off(); reject(new Error("dialog-timeout")); }, 10 * 60 * 1000);
    off = client.on("dialog.result", (d: any) => {
      if (d.requestId !== requestId) return;
      clearTimeout(timer);
      off();
      resolve(d.cancelled ? null : d.path);
    });
  });
  return { promise, cancel: () => { clearTimeout(timer); off(); } };
}

async function importProject() {
  if (picking.value) return;
  picking.value = true;
  importError.value = "";
  const requestId = crypto.randomUUID();
  const waiter = waitDialogResult(requestId);
  try {
    const res: any = await client.request("dialog.pickFolder",
                                          { purpose: "unityProject", requestId });
    if (!res.accepted) {
      waiter.cancel();
      if (res.reason !== "busy")
        importError.value = res.reason === "dialog-unavailable"
          ? "无法打开文件夹选择框" : String(res.reason);
      return;
    }
    const path = await waiter.promise;
    if (path) await selectProject(path);              // 取消：静默
  } catch (e: any) {
    waiter.cancel();
    importError.value = e?.message === "dialog-timeout"
      ? "文件夹选择超时，请重试" : String(e?.message ?? "导入失败");
  } finally {
    picking.value = false;
  }
}

async function selectProject(path: string) {
  const res: any = await client.request("project.select", { path });
  if (res.accepted) return;
  if (res.reason === "switch-confirm") {
    if (!window.confirm(`当前工程有 ${res.runningTasks} 个任务运行中，切换将终止这些任务`)) return;
    await client.request("project.switchConfirm", { path: res.newPath });
    return;
  }
  importError.value = res.reason === "invalid-project"
    ? "不是有效 Unity 工程（需含 Assets 与 ProjectSettings）"
    : String(res.reason ?? "导入失败");
}

client.on("project.state", (d: any) => { projectState.value = d; });
```

模板：`<TopBar ... :importing="picking" @import-project="importProject" />`（`picking` 时禁用导入按钮；错误文本绑定 `importError`，显示在 TopBar 下方）；`<div v-if="projectState?.state === 'activating'" class="project-banner">等待 Unity 启动（第 {{ projectState.attempt ?? 1 }} 次尝试）…</div>`，`active` 时同一位置显示工程名。

- [x] **Step 6: CMake + 测试**（`tst_project_service` 4 用例 + `tst_folder_pick` 4 用例）→ **Commit**（`feat: Unity 工程校验、原生目录选择与激活互斥切换`）

```cmake
add_executable(tst_folder_pick tests/tst_folder_pick.cpp)
target_link_libraries(tst_folder_pick PRIVATE synthforge_core Qt6::Test Qt6::WebSockets)
add_test(NAME tst_folder_pick COMMAND tst_folder_pick)
```

---

### Task 2: DirectoryWatcher + 工程树/文件操作接线

**Files:** Create `src/core/unity/DirectoryWatcher.h/.cpp`、`tests/tst_watcher.cpp`；Modify `CoreApp`（`tree.refresh`、`fs.operate` unity 分支、`file.read` unity 根、watcher 接线）、`web/src/components/FileTree.vue`。

**Interfaces:**
- `class DirectoryWatcher : QObject`：
  - `void watch(const QString& root)`（递归注册；`root` 固定为 `<project>/Assets`）。
  - `void stop()`：移除全部监控路径 + 清空 pending + 重置抑制状态（切换工程时“旧事件全丢”）。
  - `void beginSuppress()` / `void endSuppress()`：供异步 MCP 工具调用成对使用；`begin` 置深度 +1 并给 5s 上限，`end` 深度归零后追加 500ms 宽限期。
  - `bool isIgnored(const QString& path) const`；信号 `treeChanged(reason, changedDirs)`（`reason ∈ incremental|full`，`changedDirs` 为相对 root 的目录，`.` 表示根）。
- 请求 `tree.refresh {area:"unity", path?}` → 无 `path` 返回 `{ok, tree}`；有 `path` 返回 `{ok, node}`（该目录子树）；节点 `{name, path, type:"dir"|"file", children?}`，`path` 相对 `<project>/Assets`，忽略 `*.meta`。
- `fs.operate {area:"unity", ...}`：路径基准 `<project>/Assets`，op 语义与 P4 完全一致；成功后广播 `fileTree.changed {area:"unity", reason, paths:[父目录]}`（`refresh` 发 `reason:"full"`，`reveal` 不广播）。
- `file.read {area:"unity", path}`：路径基准 `<project>/Assets`，kind/语言/dataUrl 映射与 P4 Task 5 完全一致。
- watcher 事件转发：`fileTree.changed {area:"unity", reason, paths}`。

- [x] **Step 1: 写失败测试**

```cpp
#include <QTemporaryDir>
#include <QtTest>
#include "core/unity/DirectoryWatcher.h"

class WatcherTest : public QObject
{
    Q_OBJECT
private slots:
    void ignoresLibraryAndMeta()
    {
        DirectoryWatcher watcher;
        QVERIFY(watcher.isIgnored("/proj/Library/cache.bin"));
        QVERIFY(watcher.isIgnored("/proj/Assets/Player.cs.meta"));
        QVERIFY(watcher.isIgnored("/proj/Temp/xx"));
        QVERIFY(!watcher.isIgnored("/proj/Assets/Player.cs"));
    }

    void debouncesBurstIntoOneEvent()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        for (int i = 0; i < 5; ++i) {
            QFile f(dir.path() + QString("/f%1.txt").arg(i));
            f.open(QIODevice::WriteOnly);
            f.write("x");
        }
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);   // 400ms 防抖合并
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("incremental"));
    }

    void suppressWindowSwallowsEvents()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        watcher.beginSuppress();
        QFile f(dir.path() + "/mcp_wrote.txt");
        f.open(QIODevice::WriteOnly);
        f.write("x");
        QTest::qWait(600);                                  // 防抖触发时仍处抑制窗口
        watcher.endSuppress();
        QTest::qWait(800);                                  // 500ms 宽限期结束
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(WatcherTest)
#include "tst_watcher.moc"
```

- [x] **Step 2: 实现 watcher**

```cpp
void DirectoryWatcher::watch(const QString& root)
{
    stop();
    m_root = QFileInfo(root).absoluteFilePath();
    if (!QFileInfo::exists(m_root))
        return;
    registerRecursive(m_root);
}

void DirectoryWatcher::stop()
{
    m_debounce.stop();
    m_pending.clear();
    const QStringList dirs = m_fs.directories();
    if (!dirs.isEmpty())
        m_fs.removePaths(dirs);
    m_suppressDepth = 0;
    m_suppressUntilMs = 0;
    m_root.clear();
}

void DirectoryWatcher::beginSuppress()
{
    ++m_suppressDepth;
    // 长工具调用期间给足上限，避免中途掉出抑制窗口
    m_suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 5000;
}

void DirectoryWatcher::endSuppress()
{
    if (m_suppressDepth > 0)
        --m_suppressDepth;
    if (m_suppressDepth == 0)
        m_suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 500; // MCP 落盘宽限
}

bool DirectoryWatcher::isSuppressed() const
{
    return m_suppressDepth > 0
        || QDateTime::currentMSecsSinceEpoch() < m_suppressUntilMs;
}

void DirectoryWatcher::onDirectoryChanged(const QString& path)
{
    if (isIgnored(path))
        return;
    if (!m_fs.directories().contains(path))
        registerRecursive(path);                    // 新目录：始终补注册
    if (isSuppressed()) {
        m_pending.clear();                          // 抑制窗口：丢弃（MCP 定向更新负责）
        return;
    }
    m_pending.insert(path);
    m_debounce.start(400);                          // 单次定时器自动重置
}

void DirectoryWatcher::flush()
{
    if (m_pending.isEmpty())
        return;
    if (isSuppressed()) {
        m_pending.clear();
        return;
    }
    QStringList dirs;
    for (const QString& path : m_pending)
        dirs << QDir(m_root).relativeFilePath(path);   // "." 表示根
    m_pending.clear();
    if (dirs.size() > 200) {
        emit treeChanged(QStringLiteral("full"), {});   // 限流：降级全量刷新
        return;
    }
    emit treeChanged(QStringLiteral("incremental"), dirs);
}
```

> 构造函数中 `m_debounce.setSingleShot(true)` 并连接 `timeout` → `flush`；`watch()` 只负责 stop + 注册。`m_pending` 为 `QSet<QString>`；`registerRecursive` 跳过 `isIgnored` 目录。

- [x] **Step 3: CoreApp 接线（watcher / tree.refresh / fs.operate / file.read）**

watcher 事件转发：

```cpp
m_watcher = new DirectoryWatcher(this);
connect(m_watcher, &DirectoryWatcher::treeChanged, this,
        [this](const QString& reason, const QStringList& dirs) {
            m_ws->broadcastEvent("fileTree.changed",
                                 {{"area", "unity"}, {"reason", reason},
                                  {"paths", QJsonArray::fromStringList(dirs)}});
        });
```

把 Task 1 的 `stateChanged` lambda 开头补上监控同步：

```cpp
if (info.state == QStringLiteral("active"))
    m_watcher->watch(m_projects->assetsPath());
else if (info.state == QStringLiteral("deactivating"))
    m_watcher->stop();                              // 旧工程 pending 全丢
```

`tree.refresh` handler：

```cpp
static QJsonObject unityTreeNode(const QDir& assets, const QString& relative)
{
    QJsonArray children;
    const QFileInfoList entries = QDir(assets.filePath(relative))
        .entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (entry.fileName().endsWith(QLatin1String(".meta")))
            continue;
        const QString child = relative.isEmpty() ? entry.fileName()
                                                 : relative + '/' + entry.fileName();
        if (entry.isDir()) {
            children.append(unityTreeNode(assets, child));
        } else {
            children.append(QJsonObject{{"name", entry.fileName()}, {"path", child},
                                        {"type", "file"}});
        }
    }
    return QJsonObject{{"name", relative.isEmpty() ? QStringLiteral("Assets")
                                                   : QFileInfo(relative).fileName()},
                       {"path", relative}, {"type", "dir"}, {"children", children}};
}

m_router->registerHandler("tree.refresh", [this](const QJsonObject& params) {
    if (params.value("area").toString() != QLatin1String("unity"))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "area-not-supported"}});
    if (m_projects->activePath().isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "no-active-project"}});
    const QString relative = params.value("path").toString();
    if (!m_projects->isInsideAssets(m_projects->absoluteFor(relative)))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
    const QDir assets(m_projects->assetsPath());
    if (!assets.exists())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "not-found"}});
    const QJsonObject node = unityTreeNode(assets, relative);
    return QJsonValue(params.contains("path")
                          ? QJsonObject{{"ok", true}, {"node", node}}
                          : QJsonObject{{"ok", true}, {"tree", node}});
});
```

`fs.operate` 的 unity 分支（P4 handler 中 `area != "blender"` 的早退分支替换为下段；blender 分支保持 P4 实现）：

```cpp
static QString operateUnderRoot(const QString& root, const QString& op,
                                const QString& relative, const QString& name,
                                QString* changedDir)
{
    const QDir base(root);
    const QFileInfo info(base.filePath(relative));
    *changedDir = relative.isEmpty() ? QStringLiteral(".") : QFileInfo(relative).path();
    auto inside = [&root](const QString& abs) {
        const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
        QString candidate = QFileInfo(abs).canonicalFilePath();
        if (candidate.isEmpty())                     // 目标尚不存在
            candidate = QDir::cleanPath(QFileInfo(abs).absoluteDir().canonicalPath()
                                        + '/' + QFileInfo(abs).fileName());
        return !candidate.isEmpty()
            && (candidate == canonicalRoot || candidate.startsWith(canonicalRoot + '/'));
    };

    if (op == QLatin1String("mkdir")) {
        if (name.isEmpty() || name.contains('/') || name.contains('\\')
            || name == QLatin1String(".") || name == QLatin1String(".."))
            return QStringLiteral("invalid-path");
        const QString target = base.filePath(relative.isEmpty() ? name : relative + '/' + name);
        if (!inside(target))
            return QStringLiteral("invalid-path");
        if (QFileInfo::exists(target))
            return QStringLiteral("already-exists");
        return QDir().mkpath(target) ? QString() : QStringLiteral("io-error");
    }
    if (!info.exists())
        return QStringLiteral("not-found");
    if (!inside(info.absoluteFilePath()))
        return QStringLiteral("invalid-path");
    if (op == QLatin1String("rename")) {
        if (name.isEmpty() || name.contains('/') || name.contains('\\'))
            return QStringLiteral("invalid-path");
        const QString target = info.dir().filePath(name);
        if (QFileInfo::exists(target))
            return QStringLiteral("already-exists");
        return QDir().rename(info.absoluteFilePath(), target) ? QString()
                                                              : QStringLiteral("io-error");
    }
    if (op == QLatin1String("delete")) {
        const bool ok = info.isDir() ? QDir(info.absoluteFilePath()).removeRecursively()
                                     : QFile::remove(info.absoluteFilePath());
        return ok ? QString() : QStringLiteral("io-error");
    }
    if (op == QLatin1String("reveal")) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"),
                                 QDir::toNativeSeparators(info.absoluteFilePath())});
        return QString();
    }
    if (op == QLatin1String("refresh"))
        return QString();
    return QStringLiteral("unknown-op");
}

// fs.operate handler 的 unity 分支：
if (area == QLatin1String("unity")) {
    if (m_projects->activePath().isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "no-active-project"}});
    QString changedDir;
    const QString error = operateUnderRoot(m_projects->assetsPath(), op, path, name,
                                           &changedDir);
    if (!error.isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", error}});
    if (op != QLatin1String("reveal"))
        m_ws->broadcastEvent("fileTree.changed",
                             {{"area", "unity"},
                              {"reason", op == QLatin1String("refresh")
                                             ? "full" : "incremental"},
                              {"paths", QJsonArray{changedDir}}});
    return QJsonValue(QJsonObject{{"ok", true}});
}
```

`file.read` 的 unity 根解析（替换 P4 handler 的 `area != "blender"` 早退）：

```cpp
QString root;
if (area == QLatin1String("unity")) {
    if (m_projects->activePath().isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "no-active-project"}});
    root = m_projects->assetsPath();
    if (!m_projects->isInsideAssets(m_projects->absoluteFor(path)))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
} else if (area == QLatin1String("blender")) {
    if (!m_library->isValidTarget(path))
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
    root = m_library->root();
} else {
    return QJsonValue(QJsonObject{{"ok", false}, {"error", "area-not-supported"}});
}
const QString abs = QDir(root).filePath(path);
// 其余 kind/大小上限/语言映射/dataUrl 逻辑与 P4 Task 5 完全一致
```

- [x] **Step 4: FileTree.vue 的 unity 分支（局部刷新）**

保持 P4 的 blender（`assets.list`）分支不变，按 `area` 分流追加：

```ts
interface TreeNode { name: string; path: string; type: "dir" | "file"; children?: TreeNode[]; }
const tree = ref<TreeNode | null>(null);

function findNode(node: TreeNode | null, path: string): TreeNode | null {
  if (!node) return null;
  if (node.path === path) return node;
  for (const child of node.children ?? []) {
    const hit = findNode(child, path);
    if (hit) return hit;
  }
  return null;
}

async function refreshTree(path?: string) {
  const res: any = await props.client.request("tree.refresh", { area: "unity", path });
  if (!res.ok) return;
  if (path === undefined) {
    tree.value = res.tree;
    return;
  }
  const node = findNode(tree.value, path);
  if (!node) return refreshTree();               // 节点已不存在 → 全量
  node.children = res.node.children ?? [];
}

async function onUnityTreeChanged(d: any) {
  if (props.area !== "unity") return;
  if (d.reason === "full" || !Array.isArray(d.paths) || !d.paths.length)
    return refreshTree();
  for (const dir of d.paths) {
    if (dir === ".") await refreshTree();        // 根目录变化 → 全量
    else await refreshTree(dir);                 // 其余 → 只替换该目录节点
  }
}

// onMounted 内：
if (props.area === "unity") refreshTree();
props.client.on("fileTree.changed", onUnityTreeChanged);
watch(() => props.area, () => { if (props.area === "unity") refreshTree(); });
```

- [x] **Step 5: CMake + 测试 + 手工验收**

`tst_watcher` 3 用例；手工验收：

1. 在资源管理器向工程 `Assets/` 新增脚本/贴图 → 文件树只刷新对应目录；`Library/`、`*.meta` 变更无事件。
2. 右键菜单五项（新建/重命名/删除/在资源管理器显示/刷新）在 unity 区全部可用；`../` 越界返回 `invalid-path`。
3. 让 agent 通过 MCP 写脚本 → 抑制窗口内 watcher 零事件，文件树仅由 `tool.step`/定向刷新驱动。
4. 切换工程后旧工程再发生文件变更 → 无任何事件（pending 已丢弃、监控已移除）。

- [x] **Step 6: Commit**（`feat: 工程目录监控与文件树双源同步（防抖/限流/抑制窗口）`）

---

### Task 3: Unity agent 接线（运行时 + 工具裁剪 + 截图 + Console）

**Files:** Modify `CoreApp`（`m_unity` McpRuntime、`m_unityOrch`、unity `chat.send` 分支、`conn.status` 复用、bridge hooks 替换、上次工程恢复）。

**Interfaces:**
- Consumes: `McpRuntime`（P4 Task 0）、`AgentOrchestrator`（P4 Task 3）、`ProjectService`/`DirectoryWatcher`（本计划 Task 1/2）。
- **P4 Task 0 补充（本任务前置）**：`McpRuntime` 必须暴露 `McpClient* client() const`（供 orchestrator 构造）与 `void stop()`（内部 `m_client->shutdown()`，不触发自动重连；供切工程断链）；若 P4 实现时未加，先补这两个成员。
- **P4 Task 3 补充（本任务前置）**：`AgentOrchestrator` 增加 `void setConversationHistory(const QJsonArray& history)`；LLM 初始消息 = `system + 本区最近 20 条纯文本历史 + 本次任务指令`（规格 8.4）。CoreApp 在每次 `plan()/execute()` 前用 `m_sessions->recentMessages(area, 20)` 注入。
- `m_unity` 构造与 P4 的 `m_blender` 同构（`defaultBridgeCommand("unity")` + `StdioTransport`）；`m_unityTools = filterUnityTools(m_unity->tools())` 为 Unity 区工具唯一来源（保持 MCP 原始格式，用于白名单过滤与状态栏计数）；传给 `execute()` 前用 `ToolBridge::mcpToolsToOpenAi(m_unityTools)` 转成 OpenAI tools（`CoreApp.cpp` 已在 P4 `folderPick.submit` 引入该头文件）。
- `m_unityOrch` 独立实例（不能与 blender 共用，否则两区并行任务会互相覆盖 `m_area/m_taskId`）；工具钩子只挂在 unity orchestrator 上。
- unity `chat.send` 前置：`ProjectService.activePath()` 非空 + `m_unity->isReady()`；失败 `error.card {reason:"project-not-active"}` / `{reason:"unity-not-ready"}`（不消耗 LLM 调用）。
- Unity 区不经草稿/计划两步（规格 8.4）：检查通过后直接构造单组计划 `BuildPlan{{"Unity 任务", {}, "read_console 无编译错误且 Game 截图正常"}}`，`maxSteps = 50`，`execute(..., targetFolder = m_projects->assetsPath(), area = "unity")`。
- 系统提示词追加 Unity 规则段：脚本修改后必须 `read_console` 确认无编译错误；阶段收尾回传 Game 截图（`manage_editor` 截图动作）。
- 启动恢复：若 `settings.json` 的 `unity.lastProject` 仍合法 → `m_projects->select(lastProject)`（走等待 Unity 流程，不弹确认）。

- [x] **Step 1: 实现工具白名单过滤**

```cpp
QJsonArray filterUnityTools(const QJsonArray& all)
{
    static const QStringList prefixes{"manage_", "create_", "read_", "find_",
                                      "import_", "refresh_", "validate_", "batch_"};
    QJsonArray kept;
    for (const QJsonValue& value : all) {
        const QString name = value.toObject().value("name").toString();
        for (const QString& prefix : prefixes) {
            if (name.startsWith(prefix)) {
                kept.append(value);
                break;
            }
        }
    }
    return kept;
}
```

- [x] **Step 2: CoreApp 运行时与 orchestrator 接线**

`CoreApp::broadcastConnStatus` 由 P4 Task 1 提供（已含 `m_everReady`/`m_crashNotified` 维护与 `bridge-crashed` 出卡逻辑），两个运行时直接连接同一成员函数，本任务不重复定义。

统一 orchestrator 信号接线（blender 的 `m_orch` 改为 `wireOrchestrator(m_orch)`，删除 P4 Step 4 里的内联 connect，避免两套重复；函数体包含 `planReady` 的取消/暂存与 `artifactsReady` 广播）：

```cpp
void CoreApp::wireOrchestrator(AgentOrchestrator* orch)
{
    connect(orch, &AgentOrchestrator::toolStep, this,
            [this](const QString& area, const QString& tool, const QString& status) {
                m_ws->broadcastEvent("tool.step",
                                     {{"area", area}, {"tool", tool}, {"status", status}});
            });
    connect(orch, &AgentOrchestrator::delta, this,
            [this](const QString& area, const QString& text) {
                m_ws->broadcastEvent("chat.delta", {{"area", area}, {"text", text}});
            });
    connect(orch, &AgentOrchestrator::finalText, this,
            [this](const QString& area, const QString& text) {
                m_sessions->appendMessage(area, "assistant", text);
                m_ws->broadcastEvent("chat.message",
                                     {{"area", area}, {"role", "assistant"},
                                      {"content", text}});
            });
    connect(orch, &AgentOrchestrator::failed, this,
            [this](const QString& area, const QString& reason) {
                m_ws->broadcastEvent("error.card", {{"area", area}, {"reason", reason}});
            });
    connect(orch, &AgentOrchestrator::groupProgress, this,
            [this](const QString& area, int index, int count, const QString& title) {
                m_ws->broadcastEvent("task.progress",
                                     {{"area", area}, {"groupIndex", index},
                                      {"groupCount", count}, {"title", title}});
            });
    connect(orch, &AgentOrchestrator::imageReceived, this,
            [this](const QString& area, const QString& taskId, const QString& dataUrl,
                   const QString& caption) {
                m_ws->broadcastEvent("chat.image",
                                     {{"area", area}, {"taskId", taskId},
                                      {"dataUrl", dataUrl}, {"caption", caption}});
            });
    connect(orch, &AgentOrchestrator::planReady, this,
            [this](const QString& area, const QString& taskId, const QJsonObject& plan) {
                if (m_planCancelled.remove(area))
                    return;                    // draft.cancel 后迟到的计划不再弹卡
                m_pendingPlans.insert(area, plan); // plan.confirm 取用
                m_ws->broadcastEvent("plan.created",
                                     {{"area", area}, {"taskId", taskId},
                                      {"groups", plan.value("groups")},
                                      {"totalSteps", plan.value("totalSteps")}});
            });
    connect(orch, &AgentOrchestrator::artifactsReady, this,
            [this](const QString& area, const QStringList& absoluteGlbs) {
                if (!m_library || area != QLatin1String("blender"))
                    return;                    // Unity 区的产出由 AssetImporter 处理
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
}
```

创建 Unity 运行时与 orchestrator（放在 `m_blender` 之后、`project.select` handler 注册之前）：

```cpp
m_unity = new McpRuntime(QStringLiteral("unity"),
                         std::make_unique<StdioTransport>(defaultBridgeCommand("unity")),
                         this);
connect(m_unity, &McpRuntime::toolsChanged, this,
        [this](const QJsonArray& tools) {
            m_unityTools = filterUnityTools(tools);
            if (m_unity->isReady())     // 顶栏悬停文案：已就绪 · N 个工具（规格 4.2）
                broadcastConnStatus(QStringLiteral("unity"), QStringLiteral("ready"),
                                    QStringLiteral("已就绪 · %1 个工具").arg(tools.size()));
        });
connect(m_unity, &McpRuntime::statusChanged, this, &CoreApp::broadcastConnStatus);
registerBridge(QStringLiteral("unity"), [this] { return m_unity->restart(); });

m_unityOrch = new AgentOrchestrator(m_llm, m_unity->client(), this);
wireOrchestrator(m_unityOrch);
m_unityOrch->setToolCallHook([this](const QString&, bool running) {
    if (!m_watcher)
        return;
    if (running)
        m_watcher->beginSuppress();     // MCP 工具调用全程吞 watcher 回声
    else
        m_watcher->endSuppress();
});

m_projects->setBridgeHooks(
    [this] { m_everReady.remove(QStringLiteral("unity")); m_unity->stop(); }, // 主动断开：先清标记，不报 bridge-crashed
    [this] { m_unity->start(); },       // 新会话拉起（未就绪由 ProjectService 轮询）
    [this](const QString&) {
        const bool hadTasks = m_tasks->runningCount("unity") > 0;
        m_tasks->cancelArea("unity");
        if (hadTasks)
            m_ws->broadcastEvent("chat.message",
                                 {{"area", "unity"}, {"role", "assistant"},
                                  {"content", "已中断：切换工程"}});
    });
m_projects->setBridgeReadyQuery([this] { return m_unity->isReady(); });

m_unity->start();

// 启动恢复上次工程（规格 6.1 / 7.3）
const QString lastProject = m_settings->data().value("unity").toObject()
                                .value("lastProject").toString();
if (!lastProject.isEmpty() && m_projects->validate(lastProject))
    m_projects->select(lastProject);
```

- [x] **Step 3: `handleChatSend` 的 unity 分支**

插到 P4 `handleChatSend` 的 `mode` 分支之前（用户消息落库与广播复用 `appendUser` 语义）：

```cpp
if (area == QLatin1String("unity")) {
    if (m_projects->activePath().isEmpty()) {
        m_ws->broadcastEvent("error.card",
                             {{"area", "unity"}, {"reason", "project-not-active"}});
        return QJsonValue(QJsonObject{{"accepted", false}});
    }
    if (!m_unity || !m_unity->isReady()) {
        m_ws->broadcastEvent("error.card",
                             {{"area", "unity"}, {"reason", "unity-not-ready"}});
        return QJsonValue(QJsonObject{{"accepted", false}});
    }
    if (appendUser) {
        m_sessions->appendMessage(area, "user", text);
        m_ws->broadcastEvent("chat.message",
                             {{"area", area}, {"role", "user"}, {"content", text}});
    }

    const QString taskId = QStringLiteral("unity-%1").arg(++m_seq);
    const QString project = m_projects->activePath();
    const QString assetsRoot = m_projects->assetsPath();
    BuildPlan plan;
    plan.groups = {{QStringLiteral("Unity 任务"), {},
                    QStringLiteral("read_console 无编译错误且 Game 截图正常")}};
    plan.maxSteps = 50;

    m_tasks->enqueue(Task{taskId, area, project,
                          [=](const QString& tid, TaskManager* tm) {
                              m_unityOrch->setModelConfig(primaryAgentConfig());
                              m_unityOrch->setConversationHistory(
                                  m_sessions->recentMessages(area, 20));
                              m_unityOrch->execute(tid, tm,
                                                   ToolBridge::mcpToolsToOpenAi(m_unityTools),
                                                   plan, text, assetsRoot, area);
                          }});
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}
```

> Task 4 会把 enqueue 内的 `execute` 换成 `m_importer->prepare(...)` 链（先转换附件再执行）；本任务不处理 `attachments`。

- [x] **Step 4: 手工验收**

1. 导入含 MCP for Unity 包的工程 → 顶部灯绿 → 发送「创建一个红色立方体并保存场景」→ 步骤卡显示工具调用 → 对话区出现 Game 截图 → 场景中出现物体。
2. 故意让其写一个编译错误脚本 → agent 自动 `read_console` 回读并修复（观察步骤卡循环）。
3. 有运行任务时导入工程 B → 前端弹切换确认（N 个任务）→ 确认后旧任务取消并出现「已中断：切换工程」→ 灯重新亮起；无任务时不弹框直接切换。
4. 关掉 Unity 编辑器再切换到工程 B → `project.state` 显示 waiting 与尝试次数 → 打开工程后自动变为 active。
5. 重启应用 → 自动恢复到上次工程（若编辑器未开则进入 waiting）。

- [x] **Step 5: Commit**（`feat: Unity 区 agent 任务（运行时/工具白名单/截图/Console 回读）`）

---

### Task 4: AssetImporter +「添加资产」附件 chip

**Files:** Create `src/core/unity/AssetImporter.h/.cpp`、`tests/tst_asset_importer.cpp`、`web/src/components/AssetPicker.vue`；Modify `CoreApp`（unity `chat.send` 的 attachments 处理 + `asset.import` handler）、`web/src/components/ChatPanel.vue`。

**Interfaces:**
- `class AssetImporter : QObject`：
  - 构造 `AssetImporter(AssetLibrary* library, McpRuntime* blender, McpRuntime* unity, QObject* parent = nullptr)`；`void setBlenderGate(BlenderGate* gate)`（保留构造签名，避免已有测试改动）。
  - `bool hasPairedFbx(const QString& glbPath) const`；`QString fbxPathFor(const QString& glbPath) const`（同目录同名 `.fbx`）。
  - `void prepare(const QJsonArray& attachments, PrepareCallback done)`：逐个校验（存在且在库内）→ `.fbx` 直接用；`.glb` 有配对直接用；无配对则经 Blender MCP `execute_blender_code` 转换；返回 `(ok, fbx 绝对路径列表, 错误码)`。错误码：`invalid-attachment` / `blender-busy` / `blender-not-ready` / `blender-convert-failed`。
  - 门禁：一组附件中首次遇到无配对 GLB 时以 owner `"unity-convert"` 获取租约，多附件转换全程持有；`finishPrepare`（成功或失败）统一释放。检查顺序为先门禁（`blender-busy`）再 `blender-not-ready`，门禁忙时不启动转换。纯 `.fbx` / 已有配对 `.fbx` 的附件不获取门禁，不影响 Blender 区任务。
  - `void importToUnity(const QString& fbxAbsolutePath, const QJsonObject& options, std::function<void(bool, const QString&)> done)`：调 Unity MCP `import_model_file`（字段以 `tools/list` 返回的 schema 为准）。
- `chat.send` unity 分支新增 `attachments: [相对资产库路径]`：任务入队后先 `prepare`，成功则以「原文 + 附件清单（fbx 绝对路径）」执行 `m_unityOrch->execute(...)`。
- `asset.import {glbRelativePath, instanceName?, position?}`：直接导入路径（不经 agent）；成功广播 `chat.message` + `fileTree.changed {area:"unity", reason:"incremental", paths:[父目录]}`。**不新增 `asset.imported` 事件**（保持事件族固定）。
- 前端：unity 输入区 [添加资产] → `AssetPicker`（复用 P4 `assets.list`，按 `kind` 取 `glb`/`fbx`，多选）→ 输入框上方 chip；随消息发送后清空。
- 转换需要 Blender 链就绪；未就绪 → `error.card {reason:"blender-not-ready"}`（规格 6.4 的「请先启动 Blender + 重试」由 P7 错误卡渲染，用户重发即重试）。

- [x] **Step 1: 写失败测试（纯逻辑部分）**

```cpp
#include <QTemporaryDir>
#include <QtTest>
#include "core/assets/AssetLibrary.h"
#include "core/mcp/BlenderGate.h"
#include "core/unity/AssetImporter.h"

class AssetImporterTest : public QObject
{
    Q_OBJECT
private slots:
    void fbxPairDetection()
    {
        QTemporaryDir dir;
        AssetImporter importer(nullptr, nullptr, nullptr);
        const QString glb = dir.path() + "/crate.glb";
        QVERIFY(!importer.hasPairedFbx(glb));
        QFile fbx(dir.path() + "/crate.fbx");
        QVERIFY(fbx.open(QIODevice::WriteOnly));
        QVERIFY(importer.hasPairedFbx(glb));
        QCOMPARE(importer.fbxPathFor(glb), dir.path() + "/crate.fbx");
    }

    void missingAttachmentIsRejected()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        AssetImporter importer(&library, nullptr, nullptr);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("nope.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("invalid-attachment"));
    }

    void glbWithoutFbxRequiresBlender()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        AssetImporter importer(&library, nullptr, nullptr);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-not-ready"));
    }

    void glbWithoutFbxWhileGateBusy()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        BlenderGate gate;
        QVERIFY(gate.tryAcquire(QStringLiteral("blender-agent")));
        AssetImporter importer(&library, nullptr, nullptr);
        importer.setBlenderGate(&gate);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-busy"));
        QCOMPARE(gate.owner(), QStringLiteral("blender-agent")); // 不释放他人租约
    }

    void failedConversionReleasesGate()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        BlenderGate gate;
        AssetImporter importer(&library, nullptr, nullptr);
        importer.setBlenderGate(&gate);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-not-ready"));
        QVERIFY(!gate.isBusy());               // 获取后因链未就绪失败，必须释放
    }
};

QTEST_MAIN(AssetImporterTest)
#include "tst_asset_importer.moc"
```

- [x] **Step 2: 实现 AssetImporter**

```cpp
class BlenderGate;

class AssetImporter : public QObject
{
    Q_OBJECT

public:
    using PrepareCallback = std::function<void(bool ok, const QStringList& fbxPaths,
                                              const QString& error)>;

    AssetImporter(AssetLibrary* library, McpRuntime* blender, McpRuntime* unity,
                  QObject* parent = nullptr);
    void setBlenderGate(BlenderGate* gate) { m_gate = gate; }

    bool hasPairedFbx(const QString& glbPath) const;
    QString fbxPathFor(const QString& glbPath) const;
    void prepare(const QJsonArray& attachments, PrepareCallback done);
    void importToUnity(const QString& fbxAbsolutePath, const QJsonObject& options,
                       std::function<void(bool, const QString&)> done);

private:
    void continuePrepare();
    void finishPrepare(bool ok, const QString& error);
    void ensureFbx(const QString& glbAbsolutePath,
                   std::function<void(bool, const QString&, const QString&)> done);

    AssetLibrary* m_library;
    McpRuntime* m_blender;
    McpRuntime* m_unity;
    BlenderGate* m_gate = nullptr;
    bool m_gateHeld = false;
    QJsonArray m_attachments;
    int m_prepareIndex = 0;
    QStringList m_fbxPaths;
    PrepareCallback m_prepareDone;
};
```

```cpp
bool AssetImporter::hasPairedFbx(const QString& glbPath) const
{
    return QFileInfo::exists(fbxPathFor(glbPath));
}

QString AssetImporter::fbxPathFor(const QString& glbPath) const
{
    const QFileInfo info(glbPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".fbx"));
}

void AssetImporter::prepare(const QJsonArray& attachments, PrepareCallback done)
{
    m_attachments = attachments;
    m_prepareIndex = 0;
    m_fbxPaths.clear();
    m_prepareDone = std::move(done);
    continuePrepare();
}

void AssetImporter::continuePrepare()
{
    while (m_prepareIndex < m_attachments.size()) {
        const QString relative = m_attachments.at(m_prepareIndex).toString();
        const QString absolute = m_library ? m_library->absoluteFor(relative) : QString();
        if (relative.isEmpty() || !m_library || !m_library->isValidTarget(relative)
            || !QFileInfo::exists(absolute)) {
            finishPrepare(false, QStringLiteral("invalid-attachment"));
            return;
        }
        if (absolute.endsWith(QLatin1String(".fbx"), Qt::CaseInsensitive)) {
            m_fbxPaths << absolute;
            ++m_prepareIndex;
            continue;
        }
        if (!absolute.endsWith(QLatin1String(".glb"), Qt::CaseInsensitive)) {
            finishPrepare(false, QStringLiteral("invalid-attachment"));
            return;
        }
        if (hasPairedFbx(absolute)) {
            m_fbxPaths << fbxPathFor(absolute);
            ++m_prepareIndex;
            continue;
        }
        if (m_gate && !m_gateHeld) {
            // 先门禁后链就绪：忙时直接返回 blender-busy，不启动转换；成功后持有到整组附件处理完
            if (!m_gate->tryAcquire(QStringLiteral("unity-convert"))) {
                finishPrepare(false, QStringLiteral("blender-busy"));
                return;
            }
            m_gateHeld = true;
        }
        ensureFbx(absolute, [this](bool ok, const QString& fbx, const QString& error) {
            if (!ok) {
                finishPrepare(false, error);
                return;
            }
            m_fbxPaths << fbx;
            ++m_prepareIndex;
            continuePrepare();                  // 异步转换完一个，继续下一个
        });
        return;                                 // 等待转换回调
    }
    finishPrepare(true, QString());
}

void AssetImporter::finishPrepare(bool ok, const QString& error)
{
    if (m_gateHeld && m_gate)
        m_gate->release(QStringLiteral("unity-convert"));
    m_gateHeld = false;
    PrepareCallback done = m_prepareDone;
    const QStringList paths = m_fbxPaths;
    m_prepareDone = nullptr;
    m_attachments = {};
    m_prepareIndex = 0;
    if (done)
        done(ok, paths, error);
}

void AssetImporter::ensureFbx(
    const QString& glb, std::function<void(bool, const QString&, const QString&)> done)
{
    if (!m_blender || !m_blender->isReady()) {
        done(false, {}, QStringLiteral("blender-not-ready"));
        return;
    }
    const QString fbx = fbxPathFor(glb);
    const QString script =
        QStringLiteral("import bpy\n"
                       "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
                       "bpy.ops.import_scene.gltf(filepath=r'%1')\n"
                       "bpy.ops.export_scene.fbx(filepath=r'%2', use_selection=False, "
                       "apply_scale_options='FBX_SCALE_ALL', path_mode='COPY')\n")
            .arg(glb, fbx);
    m_blender->client()->callTool(
        QStringLiteral("execute_blender_code"), QJsonObject{{"code", script}},
        [done = std::move(done), fbx](const QJsonValue&, const QJsonObject& error) {
            if (!error.isEmpty() || !QFileInfo::exists(fbx)) {
                done(false, {}, QStringLiteral("blender-convert-failed"));
                return;
            }
            done(true, fbx, QString());
        });
}

void AssetImporter::importToUnity(const QString& fbx, const QJsonObject& options,
                                  std::function<void(bool, const QString&)> done)
{
    if (!m_unity || !m_unity->isReady()) {
        done(false, QStringLiteral("unity-not-ready"));
        return;
    }
    QJsonObject args{{"path", fbx}};
    if (!options.value("instanceName").toString().isEmpty())
        args.insert(QStringLiteral("name"), options.value("instanceName"));
    if (options.contains(QStringLiteral("position")))
        args.insert(QStringLiteral("position"), options.value("position"));
    m_unity->client()->callTool(
        QStringLiteral("import_model_file"), args,
        [done = std::move(done)](const QJsonValue&, const QJsonObject& error) {
            if (!error.isEmpty()) {
                done(false, QStringLiteral("import-failed"));
                return;
            }
            done(true, QString());
        });
}
```

> `prepare` 使用成员状态串行推进（unity 区任务队列本身串行），避免 `std::function` 自捕获悬垂；转换脚本对路径中的单引号需转义（Windows 常规路径不包含）。`.cpp` 需 `#include "core/mcp/BlenderGate.h"`；门禁在 `finishPrepare` 统一释放，覆盖成功、`blender-busy`、`blender-not-ready`、`blender-convert-failed` 全部出口。

- [x] **Step 3: CoreApp 接 attachments + asset.import**

创建 importer 并注入门禁（`CoreApp.h` 增加 `AssetImporter* m_importer = nullptr;`，并 `#include "core/unity/AssetImporter.h"`）：

```cpp
m_importer = new AssetImporter(m_library, m_blender, m_unity, this);
m_importer->setBlenderGate(&m_blenderGate);   // 与 P4 blender orchestrator 共用同一把门禁
```

将 Task 3 unity 分支 enqueue 内的 `execute` 替换为：

```cpp
const QJsonArray attachments = params.value("attachments").toArray();
...
m_tasks->enqueue(Task{taskId, area, project,
                      [=](const QString& tid, TaskManager* tm) {
                          m_importer->prepare(
                              attachments,
                              [=](bool ok, const QStringList& fbxPaths, const QString& error) {
                                  if (tm->isCancelled(tid)) {
                                      tm->finish(tid);
                                      return;
                                  }
                                  if (!ok) {
                                      m_ws->broadcastEvent("error.card",
                                                           {{"area", "unity"},
                                                            {"reason", error}});
                                      tm->finish(tid);
                                      return;
                                  }
                                  m_unityOrch->setModelConfig(primaryAgentConfig());
                                  m_unityOrch->setConversationHistory(
                                      m_sessions->recentMessages(area, 20));
                                  m_unityOrch->execute(tid, tm,
                                                       ToolBridge::mcpToolsToOpenAi(m_unityTools),
                                                       plan, withAttachmentBlock(text, fbxPaths),
                                                       assetsRoot, area);
                              });
                      }});
```

附件提示词拼接（`static` 辅助函数）：

```cpp
static QString withAttachmentBlock(const QString& text, const QStringList& fbxPaths)
{
    if (fbxPaths.isEmpty())
        return text;
    QString block = QStringLiteral("\n\n本次消息附带以下资产（已准备好 FBX 绝对路径）：\n");
    for (const QString& path : fbxPaths)
        block += QStringLiteral("- %1\n").arg(path);
    block += QStringLiteral("请使用 import_model_file 导入，并按用户要求实例化（命名/坐标/挂载）。");
    return text + block;
}
```

`asset.import` handler（直接导入路径）：

```cpp
m_router->registerHandler("asset.import", [this](const QJsonObject& params) {
    if (m_projects->activePath().isEmpty())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "project-not-active"}});
    if (!m_unity || !m_unity->isReady())
        return QJsonValue(QJsonObject{{"ok", false}, {"error", "unity-not-ready"}});
    const QString relative = params.value("glbRelativePath").toString();
    m_importer->prepare(
        QJsonArray{relative},
        [this, params, relative](bool ok, const QStringList& fbx, const QString& error) {
            if (!ok) {
                m_ws->broadcastEvent("error.card", {{"area", "unity"}, {"reason", error}});
                return;
            }
            m_importer->importToUnity(
                fbx.first(), params,
                [this, relative](bool imported, const QString& importError) {
                    if (!imported) {
                        m_ws->broadcastEvent("error.card",
                                             {{"area", "unity"}, {"reason", importError}});
                        return;
                    }
                    m_ws->broadcastEvent("chat.message",
                                         {{"area", "unity"}, {"role", "assistant"},
                                          {"content", QStringLiteral("已导入资产：%1")
                                               .arg(QFileInfo(relative).fileName())}});
                    m_ws->broadcastEvent(
                        "fileTree.changed",
                        {{"area", "unity"}, {"reason", "incremental"},
                         {"paths", QJsonArray{QFileInfo(relative).path()}}});
                });
        });
    return QJsonValue(QJsonObject{{"accepted", true}});
});
```

- [x] **Step 4: 前端 AssetPicker + ChatPanel chip**

`AssetPicker.vue`：

```vue
<script setup lang="ts">
import { onMounted, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

interface AssetItem { relativePath: string; name: string; kind: "glb" | "fbx"; }
const props = defineProps<{ client: WsClient }>();
const emit = defineEmits<{ close: []; confirm: [paths: string[]] }>();

const assets = ref<AssetItem[]>([]);
const selected = ref<string[]>([]);

onMounted(async () => {
  const list = (await props.client.request("assets.list", {})) as unknown as AssetItem[];
  assets.value = list.filter((a) => a.kind === "glb" || a.kind === "fbx");
});

function toggle(path: string) {
  const index = selected.value.indexOf(path);
  if (index >= 0) selected.value.splice(index, 1);
  else selected.value.push(path);
}
</script>

<template>
  <div class="modal-mask">
    <div class="modal">
      <header>添加资产（可多选）</header>
      <ul>
        <li v-for="a in assets" :key="a.relativePath"
            :class="{ picked: selected.includes(a.relativePath) }"
            @click="toggle(a.relativePath)">{{ a.relativePath }}</li>
      </ul>
      <footer>
        <button @click="emit('close')">取消</button>
        <button :disabled="!selected.length" @click="emit('confirm', selected)">添加</button>
      </footer>
    </div>
  </div>
</template>
```

`ChatPanel.vue`（在 P4 Task 6 的 `send()` 基础上追加，保留 modify 合并逻辑）：

```ts
import AssetPicker from "./AssetPicker.vue";

const attachments = ref<string[]>([]);
const pickerOpen = ref(false);

function onAssetsPicked(paths: string[]) {
  for (const path of paths)
    if (!attachments.value.includes(path)) attachments.value.push(path);
  pickerOpen.value = false;
}

function removeAttachment(path: string) {
  attachments.value = attachments.value.filter((item) => item !== path);
}

async function send() {
  const text = input.value.trim();
  if (!text && !attachments.value.length) return;
  const modify = props.area === "blender" && !!props.selectedAsset;
  const payload: any = {
    area: props.area,
    mode: modify ? "modify" : props.area === "blender" ? mode.value : undefined,
    text,
    asset: modify ? props.selectedAsset : undefined,
  };
  if (props.area === "unity" && attachments.value.length)
    payload.attachments = [...attachments.value];
  input.value = "";
  attachments.value = [];
  const res: any = await props.client.request("chat.send", payload);
  busy.value = res?.accepted !== false; // 前置失败（未激活工程/未就绪）不显示停止
}
```

```vue
<div v-if="area === 'unity' && attachments.length" class="attach-chips">
  <span v-for="path in attachments" :key="path" class="chip">
    {{ path.split("/").pop() }}
    <button @click="removeAttachment(path)">✕</button>
  </span>
</div>
<footer class="composer">
  <button v-if="area === 'unity'" class="add-asset" @click="pickerOpen = true">添加资产</button>
  <textarea v-model="input" rows="2" placeholder="描述你的需求…"
            @keydown.enter.exact.prevent="send" />
  <button @click="send">发送</button>
</footer>
<AssetPicker v-if="pickerOpen" :client="client"
             @close="pickerOpen = false" @confirm="onAssetsPicked" />
```

- [x] **Step 5: 手工验收**

1. blender 区生成 GLB（无 fbx）→ unity 区 [添加资产] 选中 → chip 出现 → 发送「放在 (1,0,2) 并命名为 Sword」→ 自动转换（Blender 需开启）→ Unity 工程内出现模型与实例 → 对话区回传结果与 Game 截图 → chip 已清空。
2. 已有 `.fbx` 配对文件时不触发 Blender 转换（观察 Blender 无操作事件）。
3. 移除 fbx 仅留 glb → 再次添加 → 重新转换成功。
4. 关闭 Blender 时发送带 glb 附件 → `error.card {reason:"blender-not-ready"}`，无 Unity 导入。
5. 多选两个 GLB → 两个 chip → 均完成转换与导入。
6. 断开 Blender 后调用 `asset.import` → `blender-not-ready`；正常时直接导入并触发文件树局部刷新。
7. Blender 区长任务运行中，从 unity 区发送无 FBX 的 GLB 附件 → `blender-busy`，无转换、无 Unity 导入；建模任务结束后重发同一消息 → 转换并导入成功。
8. Unity 区大 GLB 转换进行中，在 blender 区确认计划或发送修改指令 → `blender-busy`；计划卡保留，转换结束后再点 [开始生成] 成功。
9. 纯 `.fbx` 或已有配对 `.fbx` 的附件在 Blender 任务运行中发送 → 不触发转换，正常导入（不占用门禁）。

- [x] **Step 6: Commit**（`feat: 跨区添加资产（附件 chip / glb→fbx 转换 / unity 导入实例化）`）

---

## P6 完成标准

- [x] 互斥切换事件顺序严格为 deactivating → activating → active；有运行任务时先确认、无任务直接切换；旧任务全部取消并标记「已中断：切换工程」
- [x] 等待 Unity：未就绪时 `project.state` 带 waiting/attempt 自动重试，60s 后 `unity-not-ready`；重启应用自动恢复上次工程（`unity.lastProject`）
- [x] watcher：防抖合并、忽略规则、抑制窗口三测全绿；>200 条变更降级全量刷新；切换工程后旧工程事件全部丢弃
- [x] `tree.refresh` 可全量/单目录拉取；`fs.operate` 与 `file.read` 的 unity 分支以 `<project>/Assets` 为根且越界拒绝；文件树局部刷新
- [x] Unity agent 真实跑通创建物体 + 截图回传 + 编译错误 Console 回读自修；前置未满足时零 LLM 调用
- [x] 添加资产：chip 多选 → 发送后清空；配对直接导入 / 无配对转换 / 转换后再次添加三条路径均验证；Blender 未就绪正确报错
- [x] 转换期间持有 `BlenderGate`（owner=`unity-convert`），成功 / 失败均释放；Blender 任务运行中转换返回 `blender-busy`，反向（转换中发 Blender 任务）亦然；纯 FBX / 已有配对路径不占门禁
- [x] 导入走系统文件夹选择框：`dialog.pickFolder` → `dialog.result` → `project.select`；取消静默、busy 时不可重复弹框；headless 返回 `dialog-unavailable`
- [x] 方法/事件族仅新增 `dialog.pickFolder` / `dialog.result` 一对；切换确认仍走 `project.select` 返回值，导入结果仍用 `chat.message` + `fileTree.changed`
- [x] `tst_project_service`、`tst_folder_pick`、`tst_watcher`、`tst_asset_importer` 全绿（后者含门禁忙拒绝与失败释放两用例）

> 验收记录（2026-10-31）：自动化 ctest 34/34、vitest 177/177 全绿；Task 2/3/4 手工验收清单由用户在实机环境逐条跑通。期间实机暴露并修复了 fbx 转换回执误报与建造轮空转两个问题（`65c5480`）。
