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

## File Structure

```
src/core/settings/SettingsStore.h/.cpp   ← settings.json 读写 + 默认值（provider 表）
src/core/store/SessionStore.h/.cpp       ← SQLite：messages / drafts / tasks
src/core/tasks/TaskManager.h/.cpp        ← 每区串行队列 + 协作式取消
src/core/ws/ProtocolRouter.h/.cpp        ← method → handler 注册表
src/core/ws/WsServer.h/.cpp              ← QWebSocketServer + token + 帧编解码
src/core/llm/SseParser.h/.cpp            ← SSE 增量解析
src/core/llm/LlmGateway.h/.cpp           ← chat/completions 流式 + tool_calls 聚合
src/core/coreapp/CoreApp.h/.cpp          ← 组合根：打开存储、注册 handler、启动 WS
src/cli/main.cpp                         ← 增加 --headless（P3 起改造成 GUI 入口）
tests/tst_settings_store.cpp
tests/tst_session_store.cpp
tests/tst_task_manager.cpp
tests/tst_sse_parser.cpp
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
- `class SessionStore { explicit SessionStore(QString dbPath); bool open(); void appendMessage(area, role, content); QJsonArray messages(area) const; void saveDraft(area, content); QString draft(area) const; }`
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
Run 预期：2 用例 Passed。

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
#include <QString>
#include <functional>
#include <memory>

class TaskManager : public QObject
{
    Q_OBJECT

public:
    struct Task
    {
        QString id;
        QString area;      // "blender" | "unity"
        QString projectId; // unity 区绑定工程，可空
        std::function<void(const QString& taskId, TaskManager*)> run;
    };

    void enqueue(Task task);
    void finish(const QString& taskId);
    void cancelArea(const QString& area);
    void cancelProject(const QString& projectId);
    bool isCancelled(const QString& taskId) const;

signals:
    void started(const QString& taskId);
    void finished(const QString& taskId, bool cancelled);

private:
    void pump(const QString& area);

    QHash<QString, QQueue<Task>> m_queues;
    QHash<QString, QString> m_running;           // area -> taskId
    QHash<QString, Task> m_tasks;                // taskId -> Task（查 projectId / 取消标志）
};

inline constexpr QLatin1String kCancelledAreaPrefix = QLatin1String("cancelled:");
```

```cpp
#include "core/tasks/TaskManager.h"

void TaskManager::enqueue(Task task)
{
    m_tasks.insert(task.id, task);
    m_queues[task.area].enqueue(std::move(task));
    pump(m_queues.value(task.area).isEmpty() ? QString() : task.area == "blender"
                  ? QStringLiteral("__nop__") : QStringLiteral("__nop__"));
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
    const Task task = m_tasks.take(taskId);
    const bool cancelled = isCancelled(taskId);
    const QString area = task.area;
    if (m_running.value(area) == taskId)
        m_running.remove(area);
    emit finished(taskId, cancelled);
    pump(area);
}

void TaskManager::cancelArea(const QString& area)
{
    // 标记运行中任务
    const QString runningId = m_running.value(area);
    if (!runningId.isEmpty())
        m_tasks[runningId].id = runningId; // 保留；取消状态通过 m_cancelled 集合

    QQueue<Task> kept;
    while (!m_queues[area].isEmpty()) {
        const Task task = m_queues[area].dequeue();
        m_tasks.remove(task.id);
        emit finished(task.id, true);
    }
    // 运行中任务的取消标志由 cancelSet 提供
    m_cancelled.insert(area);
}

bool TaskManager::isCancelled(const QString& taskId) const
{
    const auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return false;
    return m_cancelled.contains(it->area);
}

void TaskManager::cancelProject(const QString& projectId)
{
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it)
        if (it->projectId == projectId)
            cancelArea(it->area);
}
```

> 注：上面 `enqueue` 中 `pump` 的调用写法需要修正为直接 `pump(task.area)`（task 已 move，先取 area 局部变量）。实现时按此修正：
>
> ```cpp
> void TaskManager::enqueue(Task task)
> {
>     const QString area = task.area;
>     m_tasks.insert(task.id, task);
>     m_queues[area].enqueue(std::move(task));
>     pump(area);
> }
> ```
>
> 且 `m_cancelled` 为 `QSet<QString>`（存 area）。`cancelArea` 中清空队列后无需保留 `kept`。

- [ ] **Step 3: CMake + 测试**（target 模板同前；预期 3 用例 Passed）

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

**Files:** Create `src/core/llm/SseParser.h/.cpp`、`src/core/llm/LlmGateway.h/.cpp`、`tests/tst_sse_parser.cpp`。

**Interfaces:**
- `class SseParser { QList<Event> feed(const QByteArray& chunk); /* Event { QString event; QByteArray data; } */ }`
- `class LlmGateway : QObject { struct StreamHandlers { std::function<void(QString delta)> onDelta; std::function<void(QJsonArray toolCalls)> onToolCalls; std::function<void(QString error)> onError; std::function<void()> onDone; }; void chatStream(LlmConfig, QJsonArray messages, QJsonArray tools, StreamHandlers); void abort(); }`
- `struct LlmConfig { QString baseUrl; QString apiKey; QString model; }`
- tool_calls 聚合结果元素：`{"id":string, "name":string, "arguments":object}`（arguments 为已解析 JSON 对象）。

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

`LlmGateway::chatStream` 核心（tool_calls 聚合）：

```cpp
void LlmGateway::chatStream(const LlmConfig& cfg, const QJsonArray& messages,
                            const QJsonArray& tools, StreamHandlers handlers)
{
    QJsonObject body{{"model", cfg.model}, {"messages", messages}, {"stream", true}};
    if (!tools.isEmpty())
        body.insert("tools", tools);

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

> 实现注意：`QHash<int, QJsonObject>` 迭代顺序无保证，tool_calls 元素需带 `index` 字段供上游排序；上述代码在 `slot.insert("index", idx)` 时补充。`abort()` 即 `m_reply->abort()`。

- [ ] **Step 3: CMake + 测试**（`tst_sse_parser` target；预期 2 用例 Passed；LlmGateway 编译通过，网络行为在 P4 用 mock LLM 验证）

- [ ] **Step 4: Commit**（`feat: SSE 增量解析与 OpenAI 兼容流式网关（tool_calls 聚合）`）

---

### Task 6: CoreApp 组装 + `--headless` + WS 集成测试

**Files:** Create `src/core/coreapp/CoreApp.h/.cpp`、`tests/tst_headless_ws.cpp`；Modify `src/cli/main.cpp`、`CMakeLists.txt`。

**Interfaces:**
- `struct CoreOptions { QString dataDir; bool headless = false; }`
- `class CoreApp : QObject { explicit CoreApp(CoreOptions, QObject* parent=nullptr); bool start(); WsServer* ws() const; }`
- 注册 handler：`settings.get` / `settings.save` / `session.load` / `session.append` / `chat.send`（P2 版：落库 user 消息 → 发 `chat.message` 事件 → 若已配置 Agent 模型则经 LlmGateway 流式回 `chat.delta`，`[DONE]` 后落库 assistant 消息；未配置则 `error.card` 事件 `{"reason":"agent-not-configured"}`）。
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

        request("session.append", {{"area", "blender"}, {"role", "user"}, {"content", "hello"}});
        const QJsonObject loaded = request("session.load", {{"area", "blender"}});
        QCOMPARE(loaded.value("result").toArray().size(), 1);

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
        return QJsonValue(m_sessions->messages(params.value("area").toString()));
    });
    m_router->registerHandler("session.append", [this](const QJsonObject& params) {
        m_sessions->appendMessage(params.value("area").toString(),
                                  params.value("role").toString(),
                                  params.value("content").toString());
        return QJsonValue(QJsonObject{{"ok", true}});
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
        const QJsonArray history = m_sessions->messages(area);

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

## P2 完成标准

- [ ] `qa` 全绿：新增 5 个测试可执行全部通过
- [ ] `synthforge_cli --headless` 启动后输出 `READY ws=...`，浏览器可用 wscat/DevTools 完成 settings.save → settings.get 往返
- [ ] `sessions.db` 生成于数据目录且为 WAL 模式；重启后消息仍在
- [ ] 未配置 Agent 密钥时 `chat.send` 触发 `error.card`（不崩溃、不调用 LLM）
- [ ] 任务取消后 `finished(cancelled=true)` 事件可观测
