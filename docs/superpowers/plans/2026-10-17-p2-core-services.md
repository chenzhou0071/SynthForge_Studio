# SynthForge Studio P2：核心服务化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 P1 内核之上组装本地服务层——WebSocket 双向协议、settings.json 持久化、SQLite 会话库、每区串行任务管理器、OpenAI 兼容 LLM 网关（SSE 流式 + tool_calls 聚合），并完成 `--headless` 集成验证。

**Architecture:** `CoreApp` 为组合根，持有 SettingsStore / SessionStore / TaskManager / LlmGateway / WsServer；WsServer 只做传输（token 握手 + 帧收发），ProtocolRouter 按 method 分发 handler；事件单向广播。线程模型：全部在 Qt 主线程 + 异步 IO，不建自建线程；任务串行由队列保证。

**Tech Stack:** P1 全部 + Qt6::WebSockets + Qt6::Sql（QSQLITE）+ Qt6::Network。

**规格映射：** 8.3 WS 协议、8.4 LLM 网关、8.5 任务管理器、7.2 存储（sessions.db）、9 错误处理（错误帧约定）。

**依赖：** P1 全部产物（`synthforge_core` 的 McpClient / StdioTransport / BridgeCommands）。

## Global Constraints

- WS 仅监听 `127.0.0.1` 随机端口；握手 URL 带随机 32 hex `token`，不符直接关闭连接。
- 帧格式：请求 `{"id":N,"method":"a.b","params":{}}` → 成功 `{"id":N,"result":{}}` / 失败 `{"id":N,"error":{"code":int,"message":string}}`；事件 `{"event":"a.b","data":{}}`（无 id）。
- 存储根：`%APPDATA%/SynthForgeStudio/`（QStandardPaths::AppDataLocation）；`settings.json` 小体量配置；`sessions.db` SQLite（WAL 模式）。
- 密钥明文仅存本机，`settings.get` 返回时不做脱敏（本机环回 + token 保护），`预览配置` 的脱敏由前端处理。
- 所有 handler 抛 `std::runtime_error` 统一转错误帧，不得让异常穿透到 Qt 事件循环。
- 任务取消为协作式：`TaskManager::isCancelled(taskId)` 轮询；取消必须发 `finished(cancelled=true)`。
- LLM 上下文只带本区最近 20 条纯文本消息（`messagePage(area, 20)`，只取 role/content，不带 id/ts），超预算从最旧截断；绝不整表加载（规格 8.4 / 4.6）。

## File Structure

```
src/core/settings/SettingsStore.h/.cpp   ← settings.json 读写 + 默认值（provider 表）
src/core/store/SessionStore.h/.cpp       ← SQLite：messages / drafts / tasks / assets
src/core/tasks/TaskManager.h/.cpp        ← 每区串行队列 + 协作式取消
src/core/ws/ProtocolRouter.h/.cpp        ← method → handler 注册表
src/core/ws/WsServer.h/.cpp              ← QWebSocketServer + token + 帧编解码
src/core/llm/SseParser.h/.cpp            ← SSE 增量解析
src/core/llm/LlmGateway.h/.cpp           ← chat/completions 流式 + tool_calls 聚合
src/core/llm/ToolBridge.h/.cpp           ← MCP tools ↔ OpenAI function calling 格式转换（纯函数）
src/core/coreapp/CoreApp.h/.cpp          ← 组合根：打开存储、注册 handler、启动 WS
src/cli/main.cpp                         ← 增加 --headless（P3 起改造成 GUI 入口）
tests/tst_settings_store.cpp
tests/tst_session_store.cpp
tests/tst_task_manager.cpp
tests/tst_sse_parser.cpp
tests/tst_tool_bridge.cpp
tests/tst_headless_ws.cpp
```

---

### Task 1: SettingsStore

**Files:** Create `src/core/settings/SettingsStore.h/.cpp`、`tests/tst_settings_store.cpp`；Modify `CMakeLists.txt`。

**Interfaces:**
- `QJsonObject makeDefaultSettings()` — 含 `providerBaseUrls`（openai/doubao/qwen/zhipu/deepseek 五个写死 Base URL）、`agent.primary` 与 `agent.summarizer`（各自 `{provider, baseUrl, apiKey, model}`）、`cloud3d`、`ui`。
- `class SettingsStore { explicit SettingsStore(QString dataDir); void load(); bool save(); QJsonObject data() const; void setData(const QJsonObject&); }`
- `QString defaultDataDir();`

- [ ] **Step 1: 写失败测试**

```cpp
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include "core/settings/SettingsStore.h"

class SettingsStoreTest : public QObject
{
    Q_OBJECT
private slots:
    void defaultsContainFiveProviders()
    {
        const QJsonObject urls =
            makeDefaultSettings().value("providerBaseUrls").toObject();
        QCOMPARE(urls.size(), 5);
        QCOMPARE(urls.value("doubao").toString(),
                 QStringLiteral("https://ark.cn-beijing.volces.com/api/v3"));
        QCOMPARE(urls.value("deepseek").toString(),
                 QStringLiteral("https://api.deepseek.com/v1"));
    }

    void saveLoadRoundTrip()
    {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        store.load();
        QJsonObject agent = store.data().value("agent").toObject();
        QJsonObject primary = agent.value("primary").toObject();
        primary.insert("model", "gpt-4o-mini");
        agent.insert("primary", primary);
        QJsonObject root = store.data();
        root.insert("agent", agent);
        store.setData(root);
        QVERIFY(store.save());

        SettingsStore reloaded(dir.path());
        reloaded.load();
        QCOMPARE(reloaded.data().value("agent").toObject().value("primary").toObject()
                     .value("model").toString(),
                 QStringLiteral("gpt-4o-mini"));
    }
};

QTEST_MAIN(SettingsStoreTest)
#include "tst_settings_store.moc"
```

- [ ] **Step 2: 实现**

`SettingsStore.h`：

```cpp
#pragma once

#include <QJsonObject>
#include <QString>

QString defaultDataDir();              // %APPDATA%/SynthForgeStudio
QJsonObject makeDefaultSettings();

class SettingsStore
{
public:
    explicit SettingsStore(QString dataDir);

    void load();                       // 文件不存在 → 写入默认值；已存在 → 与默认值做浅合并（保留未知键）
    bool save();
    QJsonObject data() const { return m_data; }
    void setData(const QJsonObject& value) { m_data = value; }

private:
    QString m_path;
    QJsonObject m_data;
};
```

`SettingsStore.cpp`（核心部分）：

```cpp
#include "core/settings/SettingsStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

QString defaultDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QJsonObject makeDefaultSettings()
{
    auto provider = [](const QString& id) {
        return QJsonObject{{"provider", id}, {"baseUrl", QString()},
                           {"apiKey", QString()}, {"model", QString()}};
    };
    return QJsonObject{
        {"providerBaseUrls",
         QJsonObject{{"openai", "https://api.openai.com/v1"},
                     {"doubao", "https://ark.cn-beijing.volces.com/api/v3"},
                     {"qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
                     {"zhipu", "https://open.bigmodel.cn/api/paas/v4"},
                     {"deepseek", "https://api.deepseek.com/v1"}}},
        {"agent", QJsonObject{{"primary", provider("openai")},
                              {"summarizer", provider("openai")}}},
        {"cloud3d", QJsonObject{{"provider", "tripo"}, {"tripo", QJsonObject{}},
                                {"hunyuan", QJsonObject{}}, {"custom", QJsonObject{}}}},
        {"ui", QJsonObject{}}};
}

SettingsStore::SettingsStore(QString dataDir)
    : m_data(makeDefaultSettings())
{
    QDir().mkpath(dataDir);
    m_path = dataDir + "/settings.json";
}

void SettingsStore::load()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        save(); // 首次运行落盘默认值
        return;
    }
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    // 浅合并：默认值兜底 + 用户已有键覆盖（未知键原样保留）
    QJsonObject merged = m_data;
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    m_data = merged;
}

bool SettingsStore::save()
{
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    return true;
}
```

- [ ] **Step 3: CMake 注册 + 跑测试**

`synthforge_core` 源文件追加 `src/core/settings/SettingsStore.cpp`；末尾追加：

```cmake
add_executable(tst_settings_store tests/tst_settings_store.cpp)
target_link_libraries(tst_settings_store PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_settings_store COMMAND tst_settings_store)
```

Run: `cmake --build build --config Debug; ctest --test-dir build -C Debug -R tst_settings_store --output-on-failure`
Expected: 2 用例 Passed。

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/core/settings tests/tst_settings_store.cpp
git commit -m "feat: settings.json 持久化与服务商默认值表"
```

---

### Task 2: SessionStore（SQLite）

**Files:** Create `src/core/store/SessionStore.h/.cpp`、`tests/tst_session_store.cpp`；Modify `CMakeLists.txt`（追加 `Qt6::Sql`）。

**Interfaces:**
- `class SessionStore { explicit SessionStore(QString dbPath); bool open(); void appendMessage(area, role, content); QJsonArray messages(area) const /*全量，仅测试/迁移用*/; MessagePage messagePage(area, limit=20, beforeId=0) const; void saveDraft(area, content); QString draft(area) const; }`
- `struct MessagePage { QJsonArray messages /*旧→新，含 id/role/content/ts*/; bool hasMore; qint64 nextCursor; }`；`limit<=0` 用默认 20，上限 100；`beforeId<=0` 取最新一页。
- 表：`messages(id INTEGER PK AUTOINCREMENT, area TEXT, role TEXT, content TEXT, ts INTEGER)`；`drafts(area TEXT PRIMARY KEY, content TEXT, updated INTEGER)`。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include "core/store/SessionStore.h"

class SessionStoreTest : public QObject
{
    Q_OBJECT
private slots:
    void messagesRoundTripPerArea()
    {
        QTemporaryDir dir;
        SessionStore store(dir.path() + "/sessions.db");
        QVERIFY(store.open());
        store.appendMessage("blender", "user", "做一个便利店");
        store.appendMessage("blender", "assistant", "好的");
        store.appendMessage("unity", "user", "建一个地面");

        const QJsonArray blenderMsgs = store.messages("blender");
        QCOMPARE(blenderMsgs.size(), 2);
        QCOMPARE(blenderMsgs.first().toObject().value("content").toString(),
                 QStringLiteral("做一个便利店"));
        QCOMPARE(store.messages("unity").size(), 1);
    }

    void draftUpsertKeepsLatest()
    {
        QTemporaryDir dir;
        SessionStore store(dir.path() + "/sessions.db");
        QVERIFY(store.open());
        store.saveDraft("blender", "第一版");
        store.saveDraft("blender", "第二版");
        QCOMPARE(store.draft("blender"), QStringLiteral("第二版"));
    }
};

QTEST_MAIN(SessionStoreTest)
#include "tst_session_store.moc"
```

- [ ] **Step 2: 实现**

```cpp
#pragma once

#include <QJsonArray>
#include <QSqlDatabase>
#include <QString>

class SessionStore
{
public:
    explicit SessionStore(QString dbPath);
    bool open();                                  // 建表 + PRAGMA journal_mode=WAL
    void appendMessage(const QString& area, const QString& role, const QString& content);
    QJsonArray messages(const QString& area) const;
    void saveDraft(const QString& area, const QString& content);
    QString draft(const QString& area) const;

private:
    QString m_path;
    QSqlDatabase m_db;
};
```

```cpp
#include "core/store/SessionStore.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSqlQuery>

SessionStore::SessionStore(QString dbPath) : m_path(std::move(dbPath)) {}

bool SessionStore::open()
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", "sessions");
    m_db.setDatabaseName(m_path);
    if (!m_db.open())
        return false;
    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode=WAL");            // 防断电损坏 + 读写并发
    q.exec("CREATE TABLE IF NOT EXISTS messages("
           "id INTEGER PRIMARY KEY AUTOINCREMENT, area TEXT NOT NULL,"
           "role TEXT NOT NULL, content TEXT NOT NULL, ts INTEGER NOT NULL)");
    q.exec("CREATE TABLE IF NOT EXISTS drafts("
           "area TEXT PRIMARY KEY, content TEXT NOT NULL, updated INTEGER NOT NULL)");
    return true;
}

void SessionStore::appendMessage(const QString& area, const QString& role,
                                 const QString& content)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO messages(area, role, content, ts) VALUES(?,?,?,?)");
    q.addBindValue(area);
    q.addBindValue(role);
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::messages(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare("SELECT role, content FROM messages WHERE area=? ORDER BY id ASC");
    q.addBindValue(area);
    q.exec();
    while (q.next())
        out.append(QJsonObject{{"role", q.value(0).toString()},
                               {"content", q.value(1).toString()}});
    return out;
}

void SessionStore::saveDraft(const QString& area, const QString& content)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO drafts(area, content, updated) VALUES(?,?,?)"
              " ON CONFLICT(area) DO UPDATE SET content=excluded.content,"
              " updated=excluded.updated");
    q.addBindValue(area);
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QString SessionStore::draft(const QString& area) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT content FROM drafts WHERE area=?");
    q.addBindValue(area);
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}
```

- [ ] **Step 3: CMake + 测试**

`find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test Sql)`；core 源文件追加 `SessionStore.cpp`，链接追加 `Qt6::Sql`；加 `tst_session_store` target 同 Task 1 模板。
Run 预期：7 用例 Passed。

第三个用例 `walModePersistedInFileHeader`：析构关闭连接后读 `sessions.db` 前 100 字节，断言偏移 18/19 均为 `2`（SQLite 文件头：2 = WAL，1 = 回滚日志）。已验证一次 RED-GREEN：注释掉 `PRAGMA journal_mode=WAL` 时 Actual=1、Expected=2 失败，恢复后 Passed。

分页用例（4 个）：`pageReturnsLatestPageAscending`（45+5 条两区穿插，limit 20 只取本区最新 20 条、升序、nextCursor=本页最早 id）、`pageWalksBackwardsWithoutGaps`（连翻三页 20/20/5，拼回 m1..m45 无重复无缺口）、`pageClampsLimit`（limit=0→20，limit=1000→100 且 hasMore）、`emptyAreaReturnsEmptyPage`（空区 hasMore=false、nextCursor=0）。分页实现要点：`SELECT id,role,content,ts ... ORDER BY id DESC LIMIT ?`（取 `limit+1` 条探测 hasMore，多的一条丢弃），结果倒序反转成旧→新；`open()` 追加 `CREATE INDEX IF NOT EXISTS idx_messages_area_id ON messages(area, id)`。

- [ ] **Step 4: Commit**（`feat: SQLite 会话存储（消息/草稿，WAL 模式）`）

---

### Task 3: TaskManager（每区串行 + 协作式取消）

**Files:** Create `src/core/tasks/TaskManager.h/.cpp`、`tests/tst_task_manager.cpp`。

**Interfaces:**
- `struct Task { QString id; QString area; QString projectId; std::function<void(const QString& taskId, TaskManager*)> run; }`
- `void enqueue(Task)`；`void finish(taskId)`；`void cancelArea(area)`；`void cancelProject(projectId)`；`bool isCancelled(taskId) const`；信号 `started(taskId)` / `finished(taskId, bool cancelled)`。
- 语义：同区 FIFO 串行；`run` 内启动异步工作后返回，由工作完成时调用 `finish(taskId)` 推进队列；`cancelArea` 清空该区未启动任务（逐个发 finished(cancelled)）并把运行中任务标记取消。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QtTest>
#include "core/tasks/TaskManager.h"

class TaskManagerTest : public QObject
{
    Q_OBJECT
private slots:
    void runsSeriallyPerArea()
    {
        TaskManager tm;
        QStringList order;
        auto make = [&](const QString& id) {
            return Task{id, "blender", "",
                        [&order](const QString& tid, TaskManager* m) {
                            order << tid;
                            m->finish(tid);
                        }};
        };
        tm.enqueue(make("t1"));
        tm.enqueue(make("t2"));
        QCOMPARE(order, (QStringList{"t1", "t2"}));
    }

    void cancelDropsQueuedAndFlagsRunning()
    {
        TaskManager tm;
        QString runningId;
        tm.enqueue(Task{"t1", "blender", "", [&](const QString& tid, TaskManager* m) {
                            runningId = tid; // 不调 finish：模拟异步进行中
                        }});
        tm.enqueue(Task{"t2", "blender", "", [](const QString&, TaskManager*) {}});
        QSignalSpy finishedSpy(&tm, &TaskManager::finished);

        tm.cancelArea("blender");

        QVERIFY(tm.isCancelled("t1"));
        QCOMPARE(finishedSpy.count(), 1); // 仅 t2 被丢弃并通知
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
    }

    void areasRunInParallel()
    {
        TaskManager tm;
        QStringList order;
        tm.enqueue(Task{"a1", "blender", "", [&](const QString&, TaskManager*) {
                            order << "a1-start";
                        }});
        tm.enqueue(Task{"u1", "unity", "", [&](const QString&, TaskManager*) {
                            order << "u1-start";
                        }});
        QCOMPARE(order, (QStringList{"a1-start", "u1-start"})); // 两区互不阻塞
    }
};

QTEST_MAIN(TaskManagerTest)
#include "tst_task_manager.moc"
```

- [ ] **Step 2: 实现**

```cpp
#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <functional>

class TaskManager;

// 顶层结构：测试与 CoreApp 直接写 Task{...}，不必带 TaskManager:: 前缀
struct Task
{
    QString id;
    QString area;      // "blender" | "unity"
    QString projectId; // unity 区绑定工程，可空
    std::function<void(const QString& taskId, TaskManager*)> run;
};

class TaskManager : public QObject
{
    Q_OBJECT

public:
    void enqueue(Task task);
    void finish(const QString& taskId);
    void cancelArea(const QString& area);
    void cancelProject(const QString& projectId);
    int runningCount(const QString& area) const;   // 运行中 + 排队（P6 切换确认用）
    bool isCancelled(const QString& taskId) const;

signals:
    void started(const QString& taskId);
    void finished(const QString& taskId, bool cancelled);

private:
    void pump(const QString& area);

    QHash<QString, QQueue<Task>> m_queues;
    QHash<QString, QString> m_running;           // area -> taskId
    QHash<QString, Task> m_tasks;                // taskId -> Task（查 projectId）
    QSet<QString> m_cancelled;                   // taskId 集合（不是 area，避免误伤后续任务）
};
```

```cpp
#include "core/tasks/TaskManager.h"

int TaskManager::runningCount(const QString& area) const
{
    return (m_running.contains(area) ? 1 : 0) + m_queues.value(area).size();
}

void TaskManager::enqueue(Task task)
{
    const QString area = task.area;
    m_tasks.insert(task.id, task);
    m_queues[area].enqueue(std::move(task));
    pump(area);
}

void TaskManager::pump(const QString& area)
{
    if (area.isEmpty() || m_running.contains(area) || m_queues[area].isEmpty())
        return;
    const Task task = m_queues[area].dequeue();
    m_running.insert(area, task.id);
    emit started(task.id);
    task.run(task.id, this);
}

void TaskManager::finish(const QString& taskId)
{
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return; // 重复 finish / 未知 id：忽略，避免重复信号
    const Task task = it.value();
    m_tasks.erase(it);

    const bool cancelled = m_cancelled.remove(taskId);
    if (m_running.value(task.area) == taskId)
        m_running.remove(task.area);
    emit finished(taskId, cancelled);
    pump(task.area);
}

void TaskManager::cancelArea(const QString& area)
{
    const QString runningId = m_running.value(area);
    if (!runningId.isEmpty())
        m_cancelled.insert(runningId);           // 运行中任务：协作式取消

    while (!m_queues[area].isEmpty()) {
        const Task task = m_queues[area].dequeue();
        m_tasks.remove(task.id);
        emit finished(task.id, true);            // 未启动任务：直接结束
    }
}

bool TaskManager::isCancelled(const QString& taskId) const
{
    return m_cancelled.contains(taskId);
}

void TaskManager::cancelProject(const QString& projectId)
{
    QStringList areas;
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it)
        if (it->projectId == projectId && !areas.contains(it->area))
            areas << it->area;                   // 先收集，避免取消过程中迭代器失效
    for (const QString& area : areas)
        cancelArea(area);
}
```

> 注：`m_cancelled` 存 taskId，`cancelArea` 只标记当前运行中任务并直接结束排队任务；同区后续新任务不会被历史取消误伤。`cancelProject` 先收集 area 再逐个取消，避免遍历 `m_tasks` 时被 `remove` 导致迭代器失效。
>
> 实际测试 5 个用例（在计划 3 个之外补了两个边界）：`cancellingRunningTaskReportsCancelledOnFinish`（取消运行中任务后，迟到的 `finish` 必须以 `cancelled=true` 上报）、`duplicateFinishIsIgnored`（重复/未知 `finish` 不再发信号，靠 `finish()` 里的 `m_tasks.find` 保护）。

- [ ] **Step 3: CMake + 测试**（target 模板同前；预期 5 用例 Passed）

- [ ] **Step 4: Commit**（`feat: 每区串行任务管理器与协作式取消`）

---

### Task 4: ProtocolRouter + WsServer

**Files:** Create `src/core/ws/ProtocolRouter.h/.cpp`、`src/core/ws/WsServer.h/.cpp`；Modify `CMakeLists.txt`（追加 `Qt6::WebSockets`）。

**Interfaces:**
- `class ProtocolRouter { using Handler = std::function<QJsonObject(const QJsonObject&)>; void registerHandler(method, Handler); QJsonValue dispatch(method, params) /*返回 result；抛 std::runtime_error 转错误帧*/ }`（`QJsonValue` 以便返回数组）
- `class WsServer : QObject { bool listen(quint16 port = 0); quint16 port() const; QString token() const; void broadcastEvent(event, data) const; }`

- [ ] **Step 1: 实现（本任务以集成测试 Task 6 覆盖，不单独写单测；ws 帧逻辑抽纯函数便于 P3 前端对拍）**

`WsServer.h`：

```cpp
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QWebSocketServer>

class ProtocolRouter;
class QWebSocket;

class WsServer : public QObject
{
    Q_OBJECT

public:
    explicit WsServer(ProtocolRouter* router, QObject* parent = nullptr);
    bool listen(quint16 port = 0);   // 只监听 127.0.0.1
    quint16 port() const { return m_server.serverPort(); }
    QString token() const { return m_token; }
    void broadcastEvent(const QString& event, const QJsonObject& data) const;

private:
    void onNewConnection();
    void onTextMessage(QWebSocket* socket, const QString& text);

    QWebSocketServer m_server;
    ProtocolRouter* m_router;
    QString m_token;
    QList<QWebSocket*> m_clients;
};
```

`WsServer.cpp` 核心：

```cpp
WsServer::WsServer(ProtocolRouter* router, QObject* parent)
    : QObject(parent),
      m_server(QStringLiteral("SynthForge"), QWebSocketServer::NonSecureMode),
      m_router(router),
      m_token(QString::fromLatin1(
          QCryptographicHash::hash(QByteArray::number(QRandomGenerator::global()->generate64()),
                                   QCryptographicHash::Sha256).toHex()))
{
    connect(&m_server, &QWebSocketServer::newConnection, this, &WsServer::onNewConnection);
}

bool WsServer::listen(quint16 port)
{
    return m_server.listen(QHostAddress::LocalHost, port);
}

void WsServer::onNewConnection()
{
    while (QWebSocket* socket = m_server.nextPendingConnection()) {
        if (socket->requestUrl().query(QUrl::FullyEncoded) != "token=" + m_token) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("bad token"));
            socket->deleteLater();
            continue;
        }
        m_clients.append(socket);
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& text) { onTextMessage(socket, text); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            m_clients.removeAll(socket);
            socket->deleteLater();
        });
    }
}

void WsServer::onTextMessage(QWebSocket* socket, const QString& text)
{
    const QJsonObject frame = QJsonDocument::fromJson(text.toUtf8()).object();
    const QJsonValue id = frame.value("id");
    const QString method = frame.value("method").toString();
    try {
        const QJsonValue result = m_router->dispatch(method, frame.value("params").toObject());
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"id", id}, {"result", result}})
                .toJson(QJsonDocument::Compact)));
    } catch (const std::exception& e) {
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"id", id},
                                      {"error", QJsonObject{{"code", -32000},
                                                            {"message", QString::fromUtf8(e.what())}}}})
                .toJson(QJsonDocument::Compact)));
    }
}

void WsServer::broadcastEvent(const QString& event, const QJsonObject& data) const
{
    const QString payload = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"event", event}, {"data", data}})
            .toJson(QJsonDocument::Compact));
    for (QWebSocket* client : m_clients)
        client->sendTextMessage(payload);
}
```

`ProtocolRouter` 实现（10 行）：查找 handler；未注册抛 `std::runtime_error("unknown method: ...")`。

- [ ] **Step 2: 构建（无独立测试，Task 6 覆盖）**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

- [ ] **Step 3: Commit**（`feat: WebSocket 服务端（token 握手 + 请求分发 + 事件广播）`）

---

### Task 5: SseParser + LlmGateway

**Files:** Create `src/core/llm/SseParser.h/.cpp`、`src/core/llm/LlmGateway.h/.cpp`、`src/core/llm/ToolBridge.h/.cpp`、`tests/tst_sse_parser.cpp`、`tests/tst_tool_bridge.cpp`。

**Interfaces:**
- `class SseParser { QList<Event> feed(const QByteArray& chunk); /* Event { QString event; QByteArray data; } */ }`
- `class LlmGateway : QObject { struct StreamHandlers { std::function<void(QString delta)> onDelta; std::function<void(QJsonArray toolCalls)> onToolCalls; std::function<void(QString error)> onError; std::function<void()> onDone; }; virtual void chatStream(LlmConfig, QJsonArray messages, QJsonArray tools, StreamHandlers); void abort(); }`
  - `chatStream` 声明为 `virtual`，仅供 P4 的 `MockLlmServer` 继承重写（按脚本喂真实 SSE 分片）；生产路径只有这一个实现。
- `struct LlmConfig { QString baseUrl; QString apiKey; QString model; }`
- `namespace ToolBridge`（纯函数，无 Qt 网络/状态依赖）：
  - `QJsonArray mcpToolsToOpenAi(const QJsonArray& mcpTools)`：MCP `{name, description, inputSchema}` → OpenAI `{type:"function", function:{name, description, parameters}}`；`inputSchema` 改名为 `parameters`，缺省时用 `{"type":"object","properties":{}}`。
  - `QJsonArray internalCallsToWire(const QJsonArray& calls)`：内部 `{id, name, arguments(object)}` → assistant 消息要求的 `{id, type:"function", function:{name, arguments:"<JSON 字符串>"}}`。
- tool_calls 聚合结果元素（**内部格式，不是 wire format**）：`{"id":string, "name":string, "arguments":object}`（arguments 为已解析 JSON 对象）。
- `chatStream` 的 `tools` 参数**必须是 OpenAI tools 格式**：调用方先经 `ToolBridge::mcpToolsToOpenAi` 转换，网关不感知 MCP schema。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QtTest>
#include "core/llm/SseParser.h"

class SseParserTest : public QObject
{
    Q_OBJECT
private slots:
    void splitsAcrossChunkBoundaries()
    {
        SseParser parser;
        QVERIFY(parser.feed("data: {\"a\":1}").isEmpty());   // 半帧不出
        const auto events = parser.feed("\n\ndata: [DONE]\n\n");
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.first().data, QByteArray("{\"a\":1}"));
        QCOMPARE(events.last().data, QByteArray("[DONE]"));
    }

    void handlesCrLfAndMultilineData()
    {
        SseParser parser;
        const auto events = parser.feed("data: line1\r\ndata: line2\r\n\r\n");
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("line1\nline2"));
    }
};

QTEST_MAIN(SseParserTest)
#include "tst_sse_parser.moc"
```

- [ ] **Step 1B: 写 ToolBridge 失败测试**

```cpp
#include <QtTest>
#include "core/llm/ToolBridge.h"

class ToolBridgeTest : public QObject
{
    Q_OBJECT
private slots:
    void mcpToolsBecomeOpenAiFunctions()
    {
        const QJsonArray mcp{
            QJsonObject{
                {"name", "execute_blender_code"},
                {"description", "执行 Blender Python 代码"},
                {"inputSchema", QJsonObject{
                    {"type", "object"},
                    {"properties", QJsonObject{
                        {"code", QJsonObject{{"type", "string"}}}
                    }}
                }}
            }
        };

        const QJsonArray tools = ToolBridge::mcpToolsToOpenAi(mcp);

        QCOMPARE(tools.size(), 1);
        const QJsonObject tool = tools.first().toObject();
        QCOMPARE(tool.value("type").toString(), QStringLiteral("function"));
        const QJsonObject fn = tool.value("function").toObject();
        QCOMPARE(fn.value("name").toString(), QStringLiteral("execute_blender_code"));
        QVERIFY(fn.contains("parameters"));      // inputSchema → parameters
        QVERIFY(!fn.contains("inputSchema"));
    }

    void internalCallsBecomeWireFormat()
    {
        const QJsonArray internal{
            QJsonObject{{"id", "call_1"},
                        {"name", "execute_blender_code"},
                        {"arguments", QJsonObject{{"code", "print(1)"}}}}};

        const QJsonArray wire = ToolBridge::internalCallsToWire(internal);

        QCOMPARE(wire.size(), 1);
        const QJsonObject call = wire.first().toObject();
        QCOMPARE(call.value("id").toString(), QStringLiteral("call_1"));
        QCOMPARE(call.value("type").toString(), QStringLiteral("function"));
        const QJsonObject fn = call.value("function").toObject();
        QCOMPARE(fn.value("name").toString(), QStringLiteral("execute_blender_code"));
        QCOMPARE(fn.value("arguments").toString(), QStringLiteral("{\"code\":\"print(1)\"}"));
    }
};

QTEST_MAIN(ToolBridgeTest)
#include "tst_tool_bridge.moc"
```

- [ ] **Step 2: 实现**

`SseParser.cpp` 核心：

```cpp
QList<SseParser::Event> SseParser::feed(const QByteArray& chunk)
{
    m_buffer += chunk;
    m_buffer.replace("\r\n", "\n");
    QList<Event> out;
    int frameEnd = -1;
    while ((frameEnd = m_buffer.indexOf("\n\n")) >= 0) {
        const QByteArray frame = m_buffer.left(frameEnd);
        m_buffer.remove(0, frameEnd + 2);
        Event event;
        for (const QByteArray& line : frame.split('\n')) {
            if (line.startsWith("event:"))
                event.event = QString::fromUtf8(line.mid(6).trimmed());
            else if (line.startsWith("data:")) {
                if (!event.data.isEmpty())
                    event.data += '\n';
                event.data += line.mid(5).trimmed();
            }
        }
        if (!event.data.isEmpty() || !event.event.isEmpty())
            out.append(event);
    }
    return out;
}
```

`ToolBridge.h`：

```cpp
#pragma once

#include <QJsonArray>

namespace ToolBridge {
QJsonArray mcpToolsToOpenAi(const QJsonArray& mcpTools);
QJsonArray internalCallsToWire(const QJsonArray& calls);
}
```

`ToolBridge.cpp`（纯函数，格式边界只在这里处理一次）：

```cpp
#include "core/llm/ToolBridge.h"

#include <QJsonDocument>
#include <QJsonObject>

QJsonArray ToolBridge::mcpToolsToOpenAi(const QJsonArray& mcpTools)
{
    QJsonArray out;
    for (const QJsonValue& value : mcpTools) {
        const QJsonObject tool = value.toObject();
        QJsonObject parameters = tool.value("inputSchema").toObject();
        if (parameters.isEmpty())
            parameters = QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}};
        out.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{{"name", tool.value("name")},
                                     {"description", tool.value("description")},
                                     {"parameters", parameters}}},
        });
    }
    return out;
}

QJsonArray ToolBridge::internalCallsToWire(const QJsonArray& calls)
{
    QJsonArray out;
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QJsonObject arguments = call.value("arguments").toObject();
        out.append(QJsonObject{
            {"id", call.value("id")},
            {"type", "function"},
            {"function", QJsonObject{
                {"name", call.value("name")},
                {"arguments", QString::fromUtf8(
                    QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}},
        });
    }
    return out;
}
```

`LlmGateway::chatStream` 核心（tool_calls 聚合）：

```cpp
void LlmGateway::chatStream(const LlmConfig& cfg, const QJsonArray& messages,
                            const QJsonArray& tools, StreamHandlers handlers)
{
    QJsonObject body{{"model", cfg.model}, {"messages", messages}, {"stream", true}};
    if (!tools.isEmpty())
        body.insert("tools", tools);   // 已是 OpenAI tools；MCP → OpenAI 由调用方经 ToolBridge 转换

    QNetworkRequest req(QUrl(cfg.baseUrl + "/chat/completions"));
    req.setRawHeader("Authorization", "Bearer " + cfg.apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    auto* reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    SseParser parser;
    QHash<int, QJsonObject> toolCalls; // index -> {id,name,arguments(字符串累加)}

    connect(reply, &QNetworkReply::readyRead, this, [=]() mutable {
        for (const auto& ev : parser.feed(reply->readAll())) {
            if (ev.data == "[DONE]") {
                QJsonArray calls;
                for (auto it = toolCalls.constBegin(); it != toolCalls.constEnd(); ++it) {
                    QJsonObject call = it.value();
                    call.insert("arguments",
                                QJsonDocument::fromJson(
                                    call.value("arguments").toString().toUtf8()).object());
                    calls.append(call);
                }
                if (!calls.isEmpty() && handlers.onToolCalls)
                    handlers.onToolCalls(calls);
                if (handlers.onDone)
                    handlers.onDone();
                return;
            }
            const QJsonObject chunk = QJsonDocument::fromJson(ev.data).object();
            const QJsonObject delta =
                chunk.value("choices").toArray().first().toObject().value("delta").toObject();
            if (delta.value("content").isString() && handlers.onDelta)
                handlers.onDelta(delta.value("content").toString());
            for (const QJsonValue& tc : delta.value("tool_calls").toArray()) {
                const QJsonObject obj = tc.toObject();
                const int idx = obj.value("index").toInt();
                QJsonObject& slot = toolCalls[idx];
                if (obj.value("id").isString())
                    slot.insert("id", obj.value("id").toString());
                const QJsonObject fn = obj.value("function").toObject();
                if (fn.value("name").isString())
                    slot.insert("name", fn.value("name").toString());
                slot.insert("arguments",
                            slot.value("arguments").toString()
                                + fn.value("arguments").toString());
            }
        }
    });
    connect(reply, &QNetworkReply::finished, this, [=]() {
        if (reply->error() != QNetworkReply::NoError && handlers.onError)
            handlers.onError(reply->errorString());
        reply->deleteLater();
    });
}
```

> 实现注意：`QHash<int, QJsonObject>` 迭代顺序无保证，tool_calls 元素需带 `index` 字段供上游排序；上述代码在 `slot.insert("index", idx)` 时补充。若服务端未返回 `id`，用 `call_<index>` 兜底，保证 assistant / tool 消息的 `tool_call_id` 可配对；`arguments` 解析失败按空对象处理，避免整轮任务失败。`abort()` 即 `m_reply->abort()`。

- [ ] **Step 3: CMake + 测试**

core 源文件追加 `src/core/llm/SseParser.cpp`、`src/core/llm/LlmGateway.cpp`、`src/core/llm/ToolBridge.cpp`；追加两个 target：

```cmake
add_executable(tst_sse_parser tests/tst_sse_parser.cpp)
target_link_libraries(tst_sse_parser PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_sse_parser COMMAND tst_sse_parser)

add_executable(tst_tool_bridge tests/tst_tool_bridge.cpp)
target_link_libraries(tst_tool_bridge PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_tool_bridge COMMAND tst_tool_bridge)
```

Run: `ctest --test-dir build -C Debug -R "tst_sse_parser|tst_tool_bridge" --output-on-failure`
Expected: `tst_sse_parser` 2 用例、`tst_tool_bridge` 2 用例，全部 Passed；LlmGateway 编译通过，网络行为在 P4 用 mock LLM 验证。

- [ ] **Step 4: Commit**（`feat: SSE 解析、OpenAI 流式网关与 MCP→Function Calling 工具桥接`）

---

### Task 6: CoreApp 组装 + `--headless` + WS 集成测试

**Files:** Create `src/core/coreapp/CoreApp.h/.cpp`、`tests/tst_headless_ws.cpp`；Modify `src/cli/main.cpp`、`CMakeLists.txt`。

**Interfaces:**
- `struct CoreOptions { QString dataDir; bool headless = false; }`
- `class CoreApp : QObject { explicit CoreApp(CoreOptions, QObject* parent=nullptr); bool start(); WsServer* ws() const; }`
- 注册 handler：`settings.get` / `settings.save` / `session.load` / `chat.send`（P2 版：落库 user 消息 → 发 `chat.message` 事件 → 若已配置 Agent 模型则经 LlmGateway 流式回 `chat.delta`，`[DONE]` 后落库 assistant 消息；未配置则 `error.card` 事件 `{"reason":"agent-not-configured"}`）。
- `session.load` 参数 `{area, limit?, beforeId?}` → `result {messages:[{id,role,content,ts}...], hasMore, nextCursor}`，内部走 `SessionStore::messagePage`，默认 20 条/页；前端滚到顶部时带 `beforeId=nextCursor` 再拉一页。
- CLI：`synthforge_cli --headless` → 以 `defaultDataDir()` 启动 CoreApp，stderr 打印 `READY ws=ws://127.0.0.1:<port>/?token=<token>`，进入事件循环。

- [ ] **Step 1: 写失败的集成测试**

```cpp
#include <QWebSocket>
#include <QTemporaryDir>
#include <QtTest>
#include "core/coreapp/CoreApp.h"
#include "core/ws/WsServer.h"

class HeadlessWsTest : public QObject
{
    Q_OBJECT
private slots:
    void settingsAndSessionRoundTrip()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());

        QWebSocket socket;
        QSignalSpy connected(&socket, &QWebSocket::connected);
        socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                             .arg(app.ws()->port()).arg(app.ws()->token())));
        QVERIFY(connected.wait(5000));

        // 请求-响应助手：同步等待指定 id 的响应帧
        int nextId = 1;
        auto request = [&](const QString& method, const QJsonObject& params) {
            QJsonObject frame{{"id", nextId}, {"method", method}, {"params", params}};
            const int expected = nextId++;
            QJsonObject received;
            auto conn = connect(&socket, &QWebSocket::textMessageReceived, this,
                                [&](const QString& text) {
                                    const QJsonObject obj =
                                        QJsonDocument::fromJson(text.toUtf8()).object();
                                    if (obj.value("id").toInt() == expected)
                                        received = obj;
                                });
            socket.sendTextMessage(QString::fromUtf8(
                QJsonDocument(frame).toJson(QJsonDocument::Compact)));
            QTRY_VERIFY(!received.isEmpty());
            disconnect(conn);
            return received;
        };

        const QJsonObject saved = request("settings.save",
            {{"agent", QJsonObject{{"primary", QJsonObject{{"provider", "deepseek"},
                                                           {"model", "deepseek-chat"}}}}}});
        QVERIFY(saved.contains("result"));

        const QJsonObject got = request("settings.get", {});
        QCOMPARE(got.value("result").toObject().value("agent").toObject()
                     .value("primary").toObject().value("model").toString(),
                 QStringLiteral("deepseek-chat"));

        // chat.send 先落库 user 消息再检查模型配置（未配置只影响 accepted，不影响持久化）
        const QJsonObject sent = request("chat.send", {{"area", "blender"}, {"text", "hello"}});
        QVERIFY(sent.value("result").toObject().contains("accepted"));
        const QJsonObject loaded = request("session.load", {{"area", "blender"}});
        const QJsonObject page = loaded.value("result").toObject();
        QCOMPARE(page.value("messages").toArray().size(), 1);
        QCOMPARE(page.value("messages").toArray().first().toObject().value("role").toString(),
                 QStringLiteral("user"));
        QCOMPARE(page.value("hasMore").toBool(), false);

        const QJsonObject unknown = request("no.such.method", {});
        QVERIFY(unknown.contains("error"));

        socket.close();
    }
};

QTEST_MAIN(HeadlessWsTest)
#include "tst_headless_ws.moc"
```

- [ ] **Step 2: 实现 CoreApp**

```cpp
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class SettingsStore;
class SessionStore;
class TaskManager;
class ProtocolRouter;
class WsServer;
class LlmGateway;

struct CoreOptions
{
    QString dataDir;
    bool headless = false;
};

class CoreApp : public QObject
{
    Q_OBJECT
public:
    explicit CoreApp(const CoreOptions& options, QObject* parent = nullptr);
    bool start();
    WsServer* ws() const { return m_ws; }

private:
    void registerHandlers();

    CoreOptions m_options;
    SettingsStore* m_settings;
    SessionStore* m_sessions;
    TaskManager* m_tasks;
    ProtocolRouter* m_router;
    WsServer* m_ws;
    LlmGateway* m_llm;
};
```

`CoreApp.cpp` 的 `start()` 与 handler 核心：

```cpp
bool CoreApp::start()
{
    m_settings->load();
    if (!m_sessions->open())
        return false;

    m_router->registerHandler("settings.get", [this](const QJsonObject&) {
        return QJsonValue(m_settings->data());
    });
    m_router->registerHandler("settings.save", [this](const QJsonObject& params) {
        m_settings->setData(params);
        if (!m_settings->save())
            throw std::runtime_error("settings save failed");
        return QJsonValue(QJsonObject{{"ok", true}});
    });
    m_router->registerHandler("session.load", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        const int limit = params.value("limit").toInt(); // 0 → 默认 20
        const qint64 beforeId = static_cast<qint64>(params.value("beforeId").toDouble());
        const MessagePage page = m_sessions->messagePage(area, limit, beforeId);
        return QJsonValue(QJsonObject{{"messages", page.messages},
                                      {"hasMore", page.hasMore},
                                      {"nextCursor", page.nextCursor}});
    });
    m_router->registerHandler("chat.send", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        const QString text = params.value("text").toString();
        m_sessions->appendMessage(area, "user", text);
        m_ws->broadcastEvent("chat.message",
                             {{"area", area}, {"role", "user"}, {"content", text}});

        const QJsonObject primary =
            m_settings->data().value("agent").toObject().value("primary").toObject();
        if (primary.value("apiKey").toString().isEmpty()
            || primary.value("model").toString().isEmpty()) {
            m_ws->broadcastEvent("error.card",
                                 {{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(QJsonObject{{"ok", false}});
        }

        LlmConfig cfg{primary.value("baseUrl").toString(),
                      primary.value("apiKey").toString(),
                      primary.value("model").toString()};
        // 规格 8.4：只带本区最近 20 条纯文本历史（含刚落库的本次 user 消息）
        QJsonArray history;
        for (const QJsonValue& value : m_sessions->messagePage(area, 20).messages) {
            const QJsonObject m = value.toObject();
            history.append(QJsonObject{{"role", m.value("role")},
                                       {"content", m.value("content")}});
        }

        m_tasks->enqueue(Task{QStringLiteral("chat-%1").arg(++m_seq), area, QString(),
                              [=](const QString& taskId, TaskManager* tm) {
                                  LlmGateway::StreamHandlers h;
                                  h.onDelta = [this, area](const QString& delta) {
                                      m_ws->broadcastEvent("chat.delta",
                                                           {{"area", area}, {"text", delta}});
                                  };
                                  h.onDone = [=]() {
                                      m_sessions->appendMessage(area, "assistant", m_streamBuf);
                                      m_ws->broadcastEvent("chat.message",
                                                           {{"area", area},
                                                            {"role", "assistant"},
                                                            {"content", m_streamBuf}});
                                      tm->finish(taskId);
                                  };
                                  h.onError = [=](const QString& err) {
                                      m_ws->broadcastEvent("error.card",
                                                           {{"area", area}, {"reason", err}});
                                      tm->finish(taskId);
                                  };
                                  m_llm->chatStream(cfg, history, {}, h);
                              }});
        return QJsonValue(QJsonObject{{"ok", true}});
    });

    if (!m_ws->listen())
        return false;
    if (m_options.headless) {
        fprintf(stderr, "READY ws=ws://127.0.0.1:%u/?token=%s\n", m_ws->port(),
                qUtf8Printable(m_ws->token()));
        fflush(stderr);
    }
    return true;
}
```

> `m_streamBuf` 为 CoreApp 成员 `QString`，在 onDelta 中累加（实现时补充 `m_streamBuf.clear()` 于入队前）。`m_seq` 为 `qint64` 自增。

CLI 增量（`src/cli/main.cpp` 顶部）：

```cpp
if (QCoreApplication::arguments().contains("--headless")) {
    QCoreApplication::setOrganizationName("SynthForge");
    QCoreApplication::setApplicationName("SynthForgeStudio");
    CoreApp app({defaultDataDir(), true});
    if (!app.start())
        return 1;
    return QCoreApplication::exec();
}
```

（放在原 QCommandLineParser 逻辑之前；原 CLI 冒烟功能保留。）

- [ ] **Step 3: CMake + 跑集成测试**

core 追加 `coreapp/CoreApp.cpp`、`ws/*.cpp`、`tasks/*.cpp`、`llm/*.cpp`、`store/*.cpp`；链接追加 `Qt6::WebSockets`、`Qt6::Network`、`Qt6::Sql`；加 `tst_headless_ws` target。

Run: `cmake --build build --config Debug; ctest --test-dir build -C Debug --output-on-failure`
Expected: 全部测试 Passed（含 settings/session 往返、未知 method 错误帧）。

- [ ] **Step 4: Commit**（`feat: CoreApp 组装与 headless WebSocket 集成链路`）

---

### Task 7: 会话持久化扩展与协议补全（chat.clear / chat.retry / bridge.restart）

**Files:**
- Modify: `src/core/store/SessionStore.h/.cpp`（补 tasks / assets 表、clearMessages；上下文复用已有 messagePage，不再新增 recentMessages）
- Modify: `src/core/coreapp/CoreApp.h/.cpp`（新增 3 个 handler、20 条上下文组装、每任务独立流缓冲）
- Modify: `tests/tst_session_store.cpp`（追加 2 用例）
- Modify: `tests/tst_headless_ws.cpp`（追加 retry/clear 用例）

**Interfaces:**
- Consumes: `SessionStore`（Task 2）、`TaskManager`（Task 3）、`CoreApp`（Task 6）。
- Produces:
  - LLM 上下文复用 Task 2 的 `messagePage(area, 20)`（取 `.messages` 后只保留 role/content），不再单独新增 `recentMessages`；分页与上下文裁剪共用同一查询。
  - `SessionStore::clearMessages(area)`、`SessionStore::upsertTask / tasks / upsertAsset / assets` — 对应规格 7.2 的「任务记录、资产卡引用」。
  - `CoreApp::registerBridge(area, std::function<bool()> restartFn)` — 由 P4（blender）/ P6（unity）注入桥接重启回调；未注册时 `bridge.restart` 返回 `bridge-not-registered`。
  - 协议：`chat.cancel {area}`（只取消本区运行中/排队任务，保留消息与重试缓存；实现为 `TaskManager::cancelArea`）、`chat.clear {area}`（取消本区任务 + 清空本区消息 + 清掉重试缓存）、`chat.retry {area}`（重走最近一次 `chat.send` 参数，不重复落库 user 消息）。
  - `chat.retry` 语义：重新提交整个任务（低模会重新规划；云任务由 P5 的 `Cloud3dService` 判断是否复用上次作业）。无重试目标时返回 `accepted=false, error=no-task-to-retry`。

- [ ] **Step 1: 写 SessionStore 扩展的失败测试**

在 `tests/tst_session_store.cpp` 的 `private slots:` 中追加：

```cpp
    void clearMessagesKeepsOtherArea()
    {
        QTemporaryDir dir;
        SessionStore store(dir.path() + "/sessions.db");
        QVERIFY(store.open());
        store.appendMessage("blender", "user", "a");
        store.appendMessage("unity", "user", "b");
        store.clearMessages("blender");
        QCOMPARE(store.messages("blender").size(), 0);
        QCOMPARE(store.messages("unity").size(), 1);
    }

    void tasksAndAssetsRoundTrip()
    {
        QTemporaryDir dir;
        SessionStore store(dir.path() + "/sessions.db");
        QVERIFY(store.open());
        store.upsertTask("t1", "blender", "lowpoly", "running", {{"text", "便利店"}});
        store.upsertTask("t1", "blender", "lowpoly", "done", {{"text", "便利店"}},
                         {{"asset", "便利店.blend"}});
        const QJsonArray tasks = store.tasks("blender");
        QCOMPARE(tasks.size(), 1);
        QCOMPARE(tasks.first().toObject().value("state").toString(), QStringLiteral("done"));
        QCOMPARE(tasks.first().toObject().value("result").toObject()
                     .value("asset").toString(),
                 QStringLiteral("便利店.blend"));

        store.upsertAsset("a1", "blender", "C:/lib/便利店/便利店.blend",
                          {{"name", "便利店"}, {"kind", "blend"}});
        const QJsonArray assets = store.assets("blender");
        QCOMPARE(assets.size(), 1);
        QCOMPARE(assets.first().toObject().value("path").toString(),
                 QStringLiteral("C:/lib/便利店/便利店.blend"));
    }
```

- [ ] **Step 2: 扩展 SessionStore**

`SessionStore.h` 整文件替换为：

```cpp
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

struct MessagePage
{
    QJsonArray messages;   // 旧→新，每项含 id / role / content / ts
    bool hasMore = false;
    qint64 nextCursor = 0;
};

class SessionStore
{
public:
    explicit SessionStore(QString dbPath);
    ~SessionStore();

    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    bool open(); // 建表 + PRAGMA journal_mode=WAL
    void appendMessage(const QString& area, const QString& role, const QString& content);
    QJsonArray messages(const QString& area) const; // 全量，仅测试/迁移用
    MessagePage messagePage(const QString& area, int limit = 20, qint64 beforeId = 0) const;
    void clearMessages(const QString& area);
    void saveDraft(const QString& area, const QString& content);
    QString draft(const QString& area) const;
    void upsertTask(const QString& id, const QString& area, const QString& kind,
                    const QString& state, const QJsonObject& payload,
                    const QJsonObject& result = QJsonObject{});
    QJsonArray tasks(const QString& area) const; // 最近 50 条，updated 降序
    void upsertAsset(const QString& id, const QString& area, const QString& path,
                     const QJsonObject& card);
    QJsonArray assets(const QString& area) const;

private:
    QString m_path;
    QString m_connectionName;
    QSqlDatabase m_db;
};
```

`SessionStore.cpp`：`open()` 中追加建表（在 drafts 建表之后）：

```cpp
    q.exec("CREATE TABLE IF NOT EXISTS tasks("
           "id TEXT PRIMARY KEY, area TEXT NOT NULL, kind TEXT NOT NULL,"
           "state TEXT NOT NULL, payload TEXT NOT NULL, result TEXT NOT NULL DEFAULT '{}',"
           "updated INTEGER NOT NULL)");
    q.exec("CREATE TABLE IF NOT EXISTS assets("
           "id TEXT PRIMARY KEY, area TEXT NOT NULL, path TEXT NOT NULL,"
           "card TEXT NOT NULL, updated INTEGER NOT NULL)");
```

文件末尾追加方法实现（新增 `#include <QJsonDocument>`）：

```cpp
void SessionStore::clearMessages(const QString& area)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM messages WHERE area=?");
    q.addBindValue(area);
    q.exec();
}

void SessionStore::upsertTask(const QString& id, const QString& area, const QString& kind,
                              const QString& state, const QJsonObject& payload,
                              const QJsonObject& result)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tasks(id, area, kind, state, payload, result, updated)"
              " VALUES(?,?,?,?,?,?,?)"
              " ON CONFLICT(id) DO UPDATE SET state=excluded.state,"
              " result=excluded.result, updated=excluded.updated");
    q.addBindValue(id);
    q.addBindValue(area);
    q.addBindValue(kind);
    q.addBindValue(state);
    q.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    q.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::tasks(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, kind, state, payload, result FROM tasks WHERE area=?"
              " ORDER BY updated DESC LIMIT 50");
    q.addBindValue(area);
    q.exec();
    while (q.next()) {
        out.append(QJsonObject{
            {"id", q.value(0).toString()},
            {"kind", q.value(1).toString()},
            {"state", q.value(2).toString()},
            {"payload", QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object()},
            {"result", QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object()}});
    }
    return out;
}

void SessionStore::upsertAsset(const QString& id, const QString& area, const QString& path,
                               const QJsonObject& card)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO assets(id, area, path, card, updated) VALUES(?,?,?,?,?)"
              " ON CONFLICT(id) DO UPDATE SET path=excluded.path,"
              " card=excluded.card, updated=excluded.updated");
    q.addBindValue(id);
    q.addBindValue(area);
    q.addBindValue(path);
    q.addBindValue(QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::assets(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, path, card FROM assets WHERE area=? ORDER BY updated DESC");
    q.addBindValue(area);
    q.exec();
    while (q.next()) {
        out.append(QJsonObject{
            {"id", q.value(0).toString()},
            {"path", q.value(1).toString()},
            {"card", QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object()}});
    }
    return out;
}
```

Run: `cmake --build build --config Debug --target tst_session_store`
Run: `ctest --test-dir build -C Debug -R tst_session_store --output-on-failure`
Expected: 5 用例 Passed。

- [ ] **Step 3: 重写 CoreApp 的 chat handler**

`CoreApp.h` 在 Task 6 版本上追加（`#include <functional>` 与 `<QHash>`）：

```cpp
public:
    using RestartFn = std::function<bool()>;
    void registerBridge(const QString& area, RestartFn fn);

private:
    QJsonValue handleChatSend(const QJsonObject& params, bool appendUser = true);
    QHash<QString, QJsonObject> m_lastSend;     // area -> 最近一次 chat.send 参数
    QHash<QString, RestartFn> m_bridgeRestarts; // area -> 桥接重启回调（P4/P6 注入）
    qint64 m_seq = 0;
```

`CoreApp.cpp` 的 `start()` 中，把 Task 6 的 `chat.send` lambda 注册**整体替换**为下面五个 handler：

```cpp
    m_router->registerHandler("chat.send", [this](const QJsonObject& params) {
        return handleChatSend(params);
    });
    m_router->registerHandler("chat.retry", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        const QJsonObject last = m_lastSend.value(area);
        if (last.isEmpty())
            return QJsonValue(QJsonObject{{"accepted", false},
                                          {"error", "no-task-to-retry"}});
        return handleChatSend(last, /*appendUser=*/false); // 不重复落库同一句 user 消息
    });
    m_router->registerHandler("chat.cancel", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        m_tasks->cancelArea(area); // 运行中任务在工具/回合边界停止，排队任务立即 finished(cancelled)
        return QJsonValue(QJsonObject{{"ok", true}});
    });
    m_router->registerHandler("chat.clear", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        m_tasks->cancelArea(area); // 正在跑的任务放弃回写
        m_sessions->clearMessages(area);
        m_lastSend.remove(area);
        return QJsonValue(QJsonObject{{"ok", true}});
    });
    m_router->registerHandler("bridge.restart", [this](const QJsonObject& params) {
        const QString area = params.value("area").toString();
        const auto fn = m_bridgeRestarts.value(area);
        if (!fn)
            return QJsonValue(QJsonObject{{"ok", false},
                                          {"error", "bridge-not-registered"}});
        return QJsonValue(QJsonObject{{"ok", fn()}});
    });
```

在 `start()` 之后追加：

```cpp
void CoreApp::registerBridge(const QString& area, RestartFn fn)
{
    m_bridgeRestarts.insert(area, std::move(fn));
}
```

`CoreApp.cpp` 末尾追加 `handleChatSend` 完整实现（替换 Task 6 中 lambda 里的原逻辑），重点变化：历史裁剪到 20 条（规格 4.6）、流式缓冲改为每任务独立 `shared_ptr<QString>`（两区可并行，不再共享 `m_streamBuf`）、任务状态落库：

```cpp
QJsonValue CoreApp::handleChatSend(const QJsonObject& params, bool appendUser)
{
    const QString area = params.value("area").toString();
    const QString text = params.value("text").toString();
    m_lastSend.insert(area, params);

    if (appendUser) {
        m_sessions->appendMessage(area, "user", text);
        m_ws->broadcastEvent("chat.message",
                             {{"area", area}, {"role", "user"}, {"content", text}});
    }

    const QJsonObject primary =
        m_settings->data().value("agent").toObject().value("primary").toObject();
    if (primary.value("apiKey").toString().isEmpty()
        || primary.value("model").toString().isEmpty()) {
        m_ws->broadcastEvent("error.card",
                             {{"area", area}, {"reason", "agent-not-configured"}});
        return QJsonValue(QJsonObject{{"accepted", false}});
    }

    LlmConfig cfg{primary.value("baseUrl").toString(),
                  primary.value("apiKey").toString(),
                  primary.value("model").toString()};
    // 规格 8.4：只带本区最近 20 条纯文本历史（含刚落库的本次 user 消息）
    QJsonArray history;
    for (const QJsonValue& value : m_sessions->messagePage(area, 20).messages) {
        const QJsonObject m = value.toObject();
        history.append(QJsonObject{{"role", m.value("role")},
                                   {"content", m.value("content")}});
    }
    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);

    m_tasks->enqueue(Task{taskId, area, QString(),
                          [=](const QString& tid, TaskManager* tm) {
                              auto buf = std::make_shared<QString>(); // 每任务独立缓冲
                              LlmGateway::StreamHandlers h;
                              h.onDelta = [this, area, buf](const QString& delta) {
                                  buf->append(delta);
                                  m_ws->broadcastEvent("chat.delta",
                                                       {{"area", area}, {"text", delta}});
                              };
                              h.onDone = [=]() {
                                  m_sessions->appendMessage(area, "assistant", *buf);
                                  m_sessions->upsertTask(tid, area, "chat", "done", params,
                                                         {{"reply", *buf}});
                                  m_ws->broadcastEvent("chat.message",
                                                       {{"area", area},
                                                        {"role", "assistant"},
                                                        {"content", *buf}});
                                  tm->finish(tid);
                              };
                              h.onError = [=](const QString& err) {
                                  m_sessions->upsertTask(tid, area, "chat", "failed", params,
                                                         {{"error", err}});
                                  m_ws->broadcastEvent("error.card",
                                                       {{"area", area}, {"reason", err}});
                                  tm->finish(tid);
                              };
                              m_sessions->upsertTask(tid, area, "chat", "running", params);
                              m_llm->chatStream(cfg, history, {}, h);
                          }});
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}
```

> 注：`chat.send` 返回的键从 `{"ok",true}` 变为 `{"accepted",true,"taskId"}`，与规格 8.3 示例一致；P2 集成测试需同步断言 `accepted`。

- [ ] **Step 4: 追加集成测试**

在 `tests/tst_headless_ws.cpp` 追加用例（复用 Task 6 的 `request` 辅助结构，或新建同类用例）：

```cpp
    void retryAndClearRoundTrip()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());

        QWebSocket socket;
        QSignalSpy connected(&socket, &QWebSocket::connected);
        socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                             .arg(app.ws()->port()).arg(app.ws()->token())));
        QVERIFY(connected.wait(5000));

        int nextId = 1;
        auto request = [&](const QString& method, const QJsonObject& params) {
            QJsonObject frame{{"id", nextId}, {"method", method}, {"params", params}};
            const int expected = nextId++;
            QJsonObject received;
            auto conn = connect(&socket, &QWebSocket::textMessageReceived, this,
                                [&](const QString& text) {
                                    const QJsonObject obj =
                                        QJsonDocument::fromJson(text.toUtf8()).object();
                                    if (obj.value("id").toInt() == expected)
                                        received = obj;
                                });
            socket.sendTextMessage(QString::fromUtf8(
                QJsonDocument(frame).toJson(QJsonDocument::Compact)));
            QTRY_VERIFY(!received.isEmpty());
            disconnect(conn);
            return received;
        };

        // 无重试目标：返回错误而不是崩溃
        const QJsonObject none = request("chat.retry", {{"area", "blender"}});
        QCOMPARE(none.value("result").toObject().value("error").toString(),
                 QStringLiteral("no-task-to-retry"));

        // 配置一个不可达的本地端点：chat.send 接受任务但异步失败
        request("settings.save",
                {{"agent", QJsonObject{{"primary",
                                        QJsonObject{{"provider", "deepseek"},
                                                    {"baseUrl", "http://127.0.0.1:1/v1"},
                                                    {"apiKey", "test"},
                                                    {"model", "deepseek-chat"}}}}}});
        const QJsonObject sent = request("chat.send", {{"area", "blender"},
                                                      {"text", "做一个便利店"}});
        QVERIFY(sent.value("result").toObject().value("accepted").toBool());
        const QString firstTask = sent.value("result").toObject().value("taskId").toString();
        QVERIFY(!firstTask.isEmpty());

        // 重试：接受新任务且不重复落库 user 消息
        const QJsonObject retried = request("chat.retry", {{"area", "blender"}});
        QVERIFY(retried.value("result").toObject().value("accepted").toBool());
        QVERIFY(retried.value("result").toObject().value("taskId").toString() != firstTask);
        QCOMPARE(request("session.load", {{"area", "blender"}})
                     .value("result").toObject().value("messages").toArray().size(),
                 1);

        // 清空：本区消息归零，且原始任务被取消
        QCOMPARE(request("chat.clear", {{"area", "blender"}})
                     .value("result").toObject().value("ok").toBool(),
                 true);
        QCOMPARE(request("session.load", {{"area", "blender"}})
                     .value("result").toObject().value("messages").toArray().size(),
                 0);

        socket.close();
    }
```

> 如果 Task 6 的 `settingsAndSessionRoundTrip` 中的 `request` 辅助是函数内 lambda，不允许跨用例复用；此用例自带一份完整 lambda（上面已给出），无需重构原用例。

- [ ] **Step 5: 构建并跑测试**

Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `tst_session_store` 全部用例（含分页）+ `tst_headless_ws` 2 用例全部 Passed。

- [ ] **Step 6: Commit**

```bash
git add src/core/store/SessionStore.h src/core/store/SessionStore.cpp src/core/coreapp/CoreApp.h src/core/coreapp/CoreApp.cpp tests/tst_session_store.cpp tests/tst_headless_ws.cpp
git commit -m "feat: 会话持久化补全（历史裁剪/清空/任务与资产卡）与 chat.retry/chat.clear/bridge.restart 协议"
```

---

## P2 完成标准

- [ ] `qa` 全绿：新增 5 个测试可执行全部通过
- [ ] `synthforge_cli --headless` 启动后输出 `READY ws=...`，浏览器可用 wscat/DevTools 完成 settings.save → settings.get 往返
- [ ] `sessions.db` 生成于数据目录且为 WAL 模式；重启后消息仍在
- [ ] `session.load` 支持游标分页：默认 20 条/页，返回 `{messages,hasMore,nextCursor}`；`beforeId` 连续翻页无重复无缺口（`tst_session_store` 4 个分页用例覆盖）
- [ ] `chat.send` 组装 LLM 上下文只取本区最近 20 条纯文本（role/content），不整表加载、不带 id/ts；长对话不会随时间无限膨胀（规格 8.4）
- [ ] 未配置 Agent 密钥时 `chat.send` 触发 `error.card`（不崩溃、不调用 LLM）
- [ ] `chat.cancel` / `chat.clear` 取消任务后 `finished(cancelled=true)` 事件可观测（消息不被清空的是 `chat.cancel`）
- [ ] `chat.clear` 清空本区消息且不影响另一区；`chat.retry` 在无目标时返回 `no-task-to-retry`，有目标时以相同参数重新提交且不重复落库 user 消息
- [ ] `bridge.restart` 在桥接未注册时返回 `bridge-not-registered`；P4 / P6 注入重启回调后由 P7 错误卡调用
