# SynthForge Studio P6：Unity 区全链实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unity 区完整闭环：工程导入校验与**全局唯一激活互斥切换**（断旧链 → 取消旧工程任务 → 切换）→ Unity agent 任务（工具裁剪 / Game 截图 / Console 回读）→ 工程目录文件监控（防抖 + 限流 + 双源去重）→「添加资产」选择器（glb→fbx 转换 → unity 导入实例化）。

**Architecture:** `ProjectService` 维护唯一激活工程与切换状态机；`DirectoryWatcher` 封装 QFileSystemWatcher（Windows 底层即 ReadDirectoryChangesW）做递归注册、400ms 防抖、事件限流与全量刷新兜底；`McpOperationWindow` 在 MCP 工具调用前后置抑制窗口，吞掉 watcher 回声事件实现双源去重；Unity agent 复用 P4 `AgentOrchestrator`，仅替换工具集与区名。

**规格映射：** 6.1 工程导入、6.2 激活互斥切换、6.3 Unity agent 流程、6.4 添加资产跨区联动、12 风险 6（watcher 溢出）。

**依赖：** P4（Orchestrator / 聊天链路 / 文件树）、P5（GLB 资产）。

## Global Constraints

- 激活互斥：任何时刻至多一个 Unity 工程处于 `active`；切换顺序固定为「旧工程 deactivating（断 MCP + 取消任务）→ 新工程 activating → active」。
- 工程校验：目录必须同时含 `Assets/` 与 `ProjectSettings/`，否则拒绝并 `error.card {reason:"invalid-project"}`。
- Watcher 忽略：路径含 `/Library/`、`/Temp/`、`/obj/`、`/Logs/`，或后缀 `.meta`。
- 防抖 400ms；单批 >200 条变更时降级为「全量刷新」单事件；抑制窗口内事件一律丢弃（MCP 自身通知负责定向更新）。
- Unity 区对话与会话存储与 blender 区完全隔离（`area:"unity"`）。

## File Structure

```
src/core/unity/ProjectService.h/.cpp    ← 校验 + 激活互斥状态机 + project.state 事件
src/core/unity/DirectoryWatcher.h/.cpp  ← 递归监控 + 防抖 + 限流 + 抑制窗口
src/core/unity/AssetImporter.h/.cpp     ← glb→fbx 转换 + unity import_model 编排
tests/tst_project_service.cpp
tests/tst_watcher.cpp
tests/tst_asset_importer.cpp
```

---

### Task 1: ProjectService（校验 + 激活互斥）

**Files:** Create `src/core/unity/ProjectService.h/.cpp`、`tests/tst_project_service.cpp`；Modify `CoreApp`（`project.select` / `project.switchConfirm` handler）。

**Interfaces:**
- `struct ProjectInfo { QString path; QString name; QString state; }`（`state ∈ inactive|deactivating|activating|active`）
- `class ProjectService : QObject { bool validate(const QString& path) const; void select(const QString& path); QString activePath() const; signals: void stateChanged(ProjectInfo); void switchRequired(QString currentPath, QString newPath); void errorOccurred(QString reason); }`
- 切换时依赖注入的取消回调：`std::function<void(const QString& projectId)> cancelProjectTasks` 与 `std::function<void()> stopUnityBridge` / `startUnityBridge`。

- [ ] **Step 1: 写失败测试**

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
};

QTEST_MAIN(ProjectServiceTest)
#include "tst_project_service.moc"
```

- [ ] **Step 2: 实现**

```cpp
bool ProjectService::validate(const QString& path) const
{
    const QDir dir(path);
    return dir.exists("Assets") && dir.exists("ProjectSettings");
}

void ProjectService::select(const QString& path)
{
    if (!validate(path)) {
        emit errorOccurred(QStringLiteral("invalid-project"));
        return;
    }
    if (path == m_active)
        return;

    if (!m_active.isEmpty()) {
        emit stateChanged({m_active, QFileInfo(m_active).fileName(), "deactivating"});
        m_hooks.cancelProjectTasks(m_active);
        m_hooks.stopBridge();
    }

    emit stateChanged({path, QFileInfo(path).fileName(), "activating"});
    m_active = path;
    m_hooks.startBridge();
    emit stateChanged({path, QFileInfo(path).fileName(), "active"});
}
```

`CoreApp` handler：`project.select {path}` → `m_projects->select(path)`；同时把 `stateChanged` 桥接为 `project.state` 事件广播前端；`errorOccurred` → `error.card`。

- [ ] **Step 3: CMake + 测试**（`tst_project_service`，2 用例）→ **Commit**（`feat: Unity 工程校验与激活互斥切换`）

---

### Task 2: DirectoryWatcher（防抖 + 限流 + 抑制窗口）

**Files:** Create `src/core/unity/DirectoryWatcher.h/.cpp`、`tests/tst_watcher.cpp`。

**Interfaces:**
- `class DirectoryWatcher : QObject { void watch(const QString& root); void stop(); void beginSuppressWindow(std::function<void()> operation); bool isIgnored(const QString& path) const; signals: void treeChanged(QString reason /* "incremental"|"full" */, QStringList changedPaths); }`
- 语义：`beginSuppressWindow(op)` 置 `m_suppressDepth++`，执行 `op()`，并在完成后追加 500ms 宽限期（MCP 落盘延迟），期间 watcher 事件丢弃。

- [ ] **Step 1: 写失败测试**

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
    }

    void suppressWindowSwallowsEvents()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        watcher.beginSuppressWindow([&] {
            QFile f(dir.path() + "/mcp_wrote.txt");
            f.open(QIODevice::WriteOnly);
            f.write("x");
            QTest::qWait(600);                              // 等 400ms 防抖触发（仍处抑制窗口）
        });
        QTest::qWait(800);                                  // 宽限期结束后
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(WatcherTest)
#include "tst_watcher.moc"
```

- [ ] **Step 2: 实现**

核心逻辑：

```cpp
void DirectoryWatcher::watch(const QString& root)
{
    stop();
    m_root = root;
    registerRecursive(root);
    connect(&m_fs, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) { onDirectoryChanged(path); });
}

void DirectoryWatcher::registerRecursive(const QString& root)
{
    m_fs.addPath(root);
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = it.next();
        if (!isIgnored(dir))
            m_fs.addPath(dir);
    }
}

void DirectoryWatcher::onDirectoryChanged(const QString& path)
{
    if (isIgnored(path))
        return;
    if (!m_fs.directories().contains(path))
        registerRecursive(path);                    // 新目录：补注册
    m_pending.insert(path);
    m_debounce.start(400);                          // 单次定时器自动重置
}

void DirectoryWatcher::flush()
{
    if (m_pending.isEmpty())
        return;
    if (QDateTime::currentMSecsSinceEpoch() < m_suppressUntilMs)
        return;                                     // 抑制窗口丢弃
    const QStringList paths(m_pending.begin(), m_pending.end());
    m_pending.clear();
    if (paths.size() > 200) {
        emit treeChanged("full", {});               // 限流：降级为全量刷新
        return;
    }
    emit treeChanged("incremental", paths);
}

void DirectoryWatcher::beginSuppressWindow(std::function<void()> operation)
{
    operation();
    m_suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 500;
}
```

`isIgnored`：规范化后检查包含子串 `"/Library/" "/Temp/" "/obj/" "/Logs/"` 或 `endsWith(".meta")`。

- [ ] **Step 3: CMake + 测试**（`tst_watcher`，3 用例；`beginSuppressWindow` 在 operation 执行期间也需抑制——实现时在调用 `operation()` 前置同一 `m_suppressUntilMs = now + 5000`，完成后改为 `now + 500`，避免长操作期间的写盘漏吞）。

- [ ] **Step 4: Commit**（`feat: 工程目录监控（防抖/限流/抑制窗口双源去重）`）

---

### Task 3: Unity agent 接线（工具裁剪 + 截图 + Console）

**Files:** Modify `CoreApp`（`chat.send area=="unity"` → 前置检查 + orchestrator 工具集）。

**Interfaces:**
- Unity 前置检查：`McpClient(unity).isReady()` 且 `ProjectService.activePath()` 非空；失败 `error.card {reason:"unity-not-ready"}`。
- 工具集：Unity MCP `tools/list` 全量（约 48 个）裁剪排除非对话所需项（如 `unity_docs`、`manage_build`），保留 `manage_*`、`create_script`、`read_console`、`manage_editor` 截图类；具体名单以 `tools/list` 实时结果与白名单前缀匹配（`manage_`、`create_`、`read_`、`find_`、`import_`、`refresh_`、`validate_`）。
- 系统提示词追加 Unity 规则段：脚本修改后必须 `read_console` 确认无编译错误；阶段收尾回传 Game 截图（`manage_editor` 截图动作）。
- Unity 区不经草稿/计划两步（规格 8.4：v1 不启用规划）：`chat.send` 检查通过后直接构造单组计划 `BuildPlan{{"unity-task", {}, "read_console 无编译错误且 Game 截图正常"}}` 并调用 `orchestrator.execute(...)`（`maxSteps` 默认 50）。

- [ ] **Step 1: 实现工具白名单过滤**

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

- [ ] **Step 2: 手工验收**

1. 导入含 MCP for Unity 包的工程 → 顶部灯绿 → 发送「创建一个红色立方体并保存场景」→ 步骤卡显示工具调用 → 对话区出现 Game 截图 → 场景中出现物体。
2. 故意让其写一个编译错误脚本 → agent 自动 `read_console` 回读并修复（观察步骤卡循环）。
3. 切换工程 B → 前端出现切换提示（`project.state` deactivating/activating）→ 旧任务被取消（`finished(cancelled=true)`）→ 灯重新亮起。

- [ ] **Step 3: Commit**（`feat: Unity 区 agent 任务（工具白名单/截图/Console 回读）`）

---

### Task 4: AssetImporter（glb→fbx→import_model）

**Files:** Create `src/core/unity/AssetImporter.h/.cpp`、`tests/tst_asset_importer.cpp`；Modify `CoreApp`（`asset.import` handler）、前端（聊天输入区「添加资产」按钮 + 资产选择器弹窗复用 FolderPicker 树）。

**Interfaces:**
- `class AssetImporter { bool hasPairedFbx(const QString& glbPath) const; QString fbxPathFor(const QString& glbPath) const; /* 同目录同名 .fbx */ }`
- `asset.import {glbRelativePath, instanceName?, position?}` 流程：配对 fbx 存在 → 直接用；否则经 Blender MCP `execute_blender_code` 转换；随后 Unity MCP `import_model`（或 `import_model_file`）导入并实例化；事件 `asset.imported {name, unityPath, screenshot?}`。

- [ ] **Step 1: 写失败测试（纯逻辑部分）**

```cpp
#include <QTemporaryDir>
#include <QtTest>
#include "core/unity/AssetImporter.h"

class AssetImporterTest : public QObject
{
    Q_OBJECT
private slots:
    void fbxPairDetection()
    {
        QTemporaryDir dir;
        AssetImporter importer;
        const QString glb = dir.path() + "/crate.glb";
        QVERIFY(!importer.hasPairedFbx(glb));
        QFile fbx(dir.path() + "/crate.fbx");
        QVERIFY(fbx.open(QIODevice::WriteOnly));
        QVERIFY(importer.hasPairedFbx(glb));
        QCOMPARE(importer.fbxPathFor(glb), dir.path() + "/crate.fbx");
    }
};

QTEST_MAIN(AssetImporterTest)
#include "tst_asset_importer.moc"
```

- [ ] **Step 2: 实现转换脚本（经 Blender MCP）**

```cpp
QString conversionScript(const QString& glbPath, const QString& fbxPath)
{
    return QStringLiteral(
        "import bpy\n"
        "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
        "bpy.ops.import_scene.gltf(filepath=r'%1')\n"
        "bpy.ops.export_scene.fbx(filepath=r'%2', use_selection=False, "
        "apply_scale_options='FBX_SCALE_ALL', path_mode='COPY')\n")
        .arg(glbPath, fbxPath);
}
```

Unity 导入：优先 `import_model_file`（直接给 fbx 绝对路径）；实例化参数用 `instanceName` + `position`（缺省原点）。

- [ ] **Step 3: 手工验收**

1. blender 区生成 GLB（无 fbx）→ unity 区输入区「添加资产」→ 选择该 GLB → 自动转换（Blender 需开启）→ Unity 工程内出现模型与实例。
2. 已有 `.fbx` 配对文件时不触发 Blender 转换（观察 Blender 无操作事件）。
3. 移除 fbx 仅留 glb → 再次添加 → 重新转换成功。

- [ ] **Step 4: Commit**（`feat: 跨区添加资产（glb→fbx 转换与 unity 导入实例化）`）

---

## P6 完成标准

- [ ] 互斥切换事件顺序严格为 deactivating → activating → active，旧任务全部取消
- [ ] watcher：防抖合并、忽略规则、抑制窗口三测全绿；>200 条变更降级全量刷新
- [ ] Unity agent 真实跑通创建物体 + 截图回传 + 编译错误 Console 回读自修
- [ ] 添加资产三条路径均验证（配对直接导入 / 无配对转换 / 转换后再次添加）
- [ ] `tst_project_service`、`tst_watcher`、`tst_asset_importer` 全绿
