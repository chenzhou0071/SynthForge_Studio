# SynthForge Studio P1：C++ 内核原型（MCP 客户端 + 冒烟 CLI）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 C++20/Qt6 从零实现一个 MCP 客户端（stdio 传输 + JSON-RPC 2.0 + initialize 握手 + tools/list + tools/call），成功拉起并驱动 `mcp-for-blender` 与 `mcp-for-unity` 两个真实桥接进程。

**Architecture:** 三层解耦——`McpTransport`（抽象传输接口）← `StdioTransport`（QProcess 进程与管道，行缓冲切分）；`McpClient`（JSON-RPC 请求-响应匹配、超时、MCP 握手状态机）；`synthforge-cli`（演示壳，做真实链路的端到端冒烟）。测试不依赖 Python：单元测试用内存 `FakeTransport`，集成测试用 C++ 编写的 `mock_mcp_server` 子进程。

**Tech Stack:** C++20、Qt 6（Core / Test）、CMake 3.21+、MSVC、CTest。

**计划范围（路线图）：**

| 阶段 | 内容 | 计划文件 |
|---|---|---|
| P1 | C++ 内核原型：MCP 客户端 + 冒烟 CLI | **本文件** |
| P2 | 核心服务化：WS 协议 + `--headless` + 任务/设置模块 + SQLite 会话 + LLM 网关 | [2026-10-17-p2-core-services.md](2026-10-17-p2-core-services.md) |
| P3 | 前端骨架：Vite/Vue3 + 布局四件套 + WebEngine 壳 | [2026-10-17-p3-frontend-skeleton.md](2026-10-17-p3-frontend-skeleton.md) |
| P4 | 低模建模全链：总结 agent 草稿流 → 确认目录 → Blender 循环 → 落盘预览 | [2026-10-17-p4-lowpoly-pipeline.md](2026-10-17-p4-lowpoly-pipeline.md) |
| P5 | 云 API 模式：Tripo / 混元 TC3 / 自定义 | [2026-10-17-p5-cloud-3d.md](2026-10-17-p5-cloud-3d.md) |
| P6 | Unity 区全链：工程互斥激活、文件双源同步、添加资产 | [2026-10-17-p6-unity-area.md](2026-10-17-p6-unity-area.md) |
| P7 | 打磨打包：错误卡、环境自检、windeployqt 单包 | [2026-10-17-p7-polish-package.md](2026-10-17-p7-polish-package.md) |

## Global Constraints

- **平台**：Windows 10/11 x64；统一 MSVC 工具链（P3 起 Qt WebEngine 强制要求，P1 起即统一）。
- **语言/框架**：C++20；Qt 6.5+；核心代码除 Qt 外不引入第三方库，JSON 一律用 QJsonDocument。
- **MCP 传输**：stdio，换行分隔 JSON-RPC 2.0；**stdout 只承载协议数据**，stderr 全部转发日志，绝不混流。
- **默认桥接命令**（写死默认值，P2 起可在配置中覆盖）：
  - blender：`uvx --python 3.11 mcp-for-blender`，附带环境变量 `UV_PYTHON_PREFERENCE=only-managed`
  - unity：`uvx --from mcpforunityserver mcp-for-unity --transport stdio`
- **uvx 路径**：`program` 字段默认 `"uvx"`，走 PATH 查找；不在代码中写死绝对路径。
- **命名规范**：类 PascalCase、成员函数 camelCase、成员变量 `m_` 前缀；文件名与类同名。
- **提交信息前缀**：`feat` / `fix` / `test` / `chore` / `docs`。
- **协议版本**：initialize 发送 `protocolVersion: "2025-06-18"`，以服务端返回值为准（不强行校验）。

## File Structure

```
CMakeLists.txt                        ← 唯一构建入口；每个 Task 增量添加 target
src/
  core/
    mcp/
      McpTransport.h                  ← 抽象传输接口（信号：started / lineReceived / stderrChunk / stopped）
      JsonRpcLineCodec.h/.cpp         ← 行解析（Response / ErrorResponse / Notification）与请求序列化
      McpClient.h/.cpp                ← MCP 握手 + 请求-响应匹配 + 超时
      StdioTransport.h/.cpp           ← QProcess 生命周期 + 行缓冲切分 + stderr 转发
      BridgeCommands.h/.cpp           ← 两条默认桥接命令的映射表
  cli/
    main.cpp                          ← 冒烟 CLI（QCommandLineParser + 事件循环）
tests/
  mock/
    mock_mcp_server.cpp               ← 测试用 stdio mock server（独立可执行，纯 C++ + Qt Core）
  tst_smoke.cpp                       ← 工具链冒烟（Task 1）
  tst_jsonrpc.cpp                     ← codec 单元测试（Task 2）
  tst_mcpclient.cpp                   ← McpClient + FakeTransport 单元测试（Task 3）
  tst_stdio_transport.cpp             ← 真实子进程集成测试（Task 4）
  tst_bridge_commands.cpp             ← 默认命令映射测试（Task 5）
```

**依赖方向**：`cli → core`；`tests → core`；core 内部 `McpClient → McpTransport → (StdioTransport)`，无环。

---

### Task 1: 工程骨架与测试基础设施

**Files:**
- Create: `CMakeLists.txt`
- Create: `tests/tst_smoke.cpp`

**Interfaces:**
- Consumes: 无（全新工程）
- Produces: CMake 目标命名约定（`synthforge_core` 静态库、`tst_*` 测试可执行）；后续所有 Task 均向此文件增量添加 target。

- [ ] **Step 1: 确认本机 Qt 安装位置**

Run: `dir C:\Qt`
Expected: 看到形如 `6.8.x` 的版本目录，其下有 `msvc2022_64`（或 `msvc2019_64`）子目录。记下完整路径，后续命令中的 `<QT_KIT>` 均替换为它（例如 `C:/Qt/6.8.3/msvc2022_64`）。

- [ ] **Step 2: 写失败的冒烟测试**

`tests/tst_smoke.cpp`：

```cpp
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class SmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void qtJsonRoundTripWorks()
    {
        const QJsonObject obj{{QStringLiteral("name"), QStringLiteral("synthforge")},
                              {QStringLiteral("version"), 1}};
        const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        const QJsonObject back = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(back.value(QStringLiteral("name")).toString(), QStringLiteral("synthforge"));
    }
};

QTEST_MAIN(SmokeTest)
#include "tst_smoke.moc"
```

- [ ] **Step 3: 写最小 CMakeLists（只含冒烟测试）**

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.21)
project(SynthForgeStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test)

enable_testing()

add_executable(tst_smoke tests/tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_smoke COMMAND tst_smoke)
```

- [ ] **Step 4: 配置并构建**

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Expected: 编译通过，产出 `build/Debug/tst_smoke.exe`。

- [ ] **Step 5: 运行测试，确认通过（此时测试是"真通过"而非"真失败"——骨架验证，不适用先红后绿）**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `1/1 Test #1: tst_smoke ... Passed`。

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/tst_smoke.cpp
git commit -m "chore: 初始化 CMake 工程骨架与 Qt Test 冒烟测试"
```

---

### Task 2: JsonRpcLineCodec（行编解码）

**Files:**
- Create: `src/core/mcp/JsonRpcLineCodec.h`
- Create: `src/core/mcp/JsonRpcLineCodec.cpp`
- Create: `tests/tst_jsonrpc.cpp`
- Modify: `CMakeLists.txt`（新增 `synthforge_core` 库与 `tst_jsonrpc` target）

**Interfaces:**
- Consumes: Task 1 的 CMake 骨架。
- Produces:
  - `struct JsonRpcLine { enum class Kind { Response, ErrorResponse, Notification }; Kind kind; qint64 id; QJsonValue result; QJsonObject error; QString method; QJsonValue params; };`（`params` 保留原始形态：JSON-RPC 2.0 允许对象或数组）
  - `parse` 校验入站消息的 `jsonrpc` 必须为 `"2.0"`，否则拒绝并写入 error。
  - `static bool JsonRpcLineCodec::parse(const QByteArray& line, JsonRpcLine* out, QString* error)`
  - `static QByteArray JsonRpcLineCodec::makeRequest(qint64 id, const QString& method, const QJsonObject& params)`（返回值含尾部 `\n`）
  - `static QByteArray JsonRpcLineCodec::makeNotification(const QString& method, const QJsonObject& params)`（返回值含尾部 `\n`）

- [ ] **Step 1: 写失败的测试**

`tests/tst_jsonrpc.cpp`：

```cpp
#include <QJsonDocument>
#include <QtTest>
#include "core/mcp/JsonRpcLineCodec.h"

class JsonRpcTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesSuccessResponse()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(JsonRpcLineCodec::parse(
            R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})", &msg, &err));
        QCOMPARE(msg.kind, JsonRpcLine::Kind::Response);
        QCOMPARE(msg.id, qint64(7));
        QCOMPARE(msg.result.toObject().value(QStringLiteral("ok")).toBool(), true);
    }

    void parsesErrorResponse()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(JsonRpcLineCodec::parse(
            R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"method not found"}})",
            &msg, &err));
        QCOMPARE(msg.kind, JsonRpcLine::Kind::ErrorResponse);
        QCOMPARE(msg.id, qint64(3));
        QCOMPARE(msg.error.value(QStringLiteral("code")).toInt(), -32601);
    }

    void parsesNotification()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(JsonRpcLineCodec::parse(
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info"}})",
            &msg, &err));
        QCOMPARE(msg.kind, JsonRpcLine::Kind::Notification);
        QCOMPARE(msg.method, QStringLiteral("notifications/message"));
        QCOMPARE(msg.params.toObject().value(QStringLiteral("level")).toString(),
                 QStringLiteral("info"));
    }

    void parsesNotificationWithArrayParams()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(JsonRpcLineCodec::parse(
            R"({"jsonrpc":"2.0","method":"notifications/custom","params":[1,"two"]})",
            &msg, &err));
        QCOMPARE(msg.kind, JsonRpcLine::Kind::Notification);
        QVERIFY(msg.params.isArray());
        QCOMPARE(msg.params.toArray().size(), 2);
        QCOMPARE(msg.params.toArray().at(0).toInt(), 1);
    }

    void rejectsWrongJsonrpcVersion()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(!JsonRpcLineCodec::parse(
            R"({"jsonrpc":"1.0","id":1,"result":{}})", &msg, &err));
        QVERIFY(!err.isEmpty());
    }

    void rejectsMalformedJson()
    {
        JsonRpcLine msg;
        QString err;
        QVERIFY(!JsonRpcLineCodec::parse("not json at all", &msg, &err));
        QVERIFY(!err.isEmpty());
    }

    void serializesRequestWithTrailingNewline()
    {
        const QByteArray line = JsonRpcLineCodec::makeRequest(
            5, QStringLiteral("tools/list"), QJsonObject{});
        QVERIFY(line.endsWith('\n'));
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        QCOMPARE(obj.value(QStringLiteral("id")).toInt(), 5);
        QCOMPARE(obj.value(QStringLiteral("method")).toString(), QStringLiteral("tools/list"));
        QCOMPARE(obj.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    }

    void serializesNotificationWithoutId()
    {
        const QByteArray line = JsonRpcLineCodec::makeNotification(
            QStringLiteral("notifications/initialized"), QJsonObject{});
        QVERIFY(line.endsWith('\n'));
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        QVERIFY(!obj.contains(QStringLiteral("id")));
        QCOMPARE(obj.value(QStringLiteral("method")).toString(),
                 QStringLiteral("notifications/initialized"));
    }
};

QTEST_MAIN(JsonRpcTest)
#include "tst_jsonrpc.moc"
```

- [ ] **Step 2: 运行测试确认失败（编译报错：找不到 JsonRpcLineCodec.h）**

Run: `cmake --build build --config Debug --target tst_jsonrpc`
Expected: 编译失败，报 `JsonRpcLineCodec.h: No such file or directory`（若提示 target 不存在，先执行 Step 4 的 CMake 注册再重试）。

- [ ] **Step 3: 写最小实现**

`src/core/mcp/JsonRpcLineCodec.h`：

```cpp
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

struct JsonRpcLine
{
    enum class Kind { Response, ErrorResponse, Notification };

    Kind kind = Kind::Response;
    qint64 id = 0;          // Response / ErrorResponse 有效
    QJsonValue result;      // Response
    QJsonObject error;      // ErrorResponse：{code, message, data?}
    QString method;         // Notification
    QJsonValue params;      // Notification（对象或数组，保持原始 JSON 形态）
};

class JsonRpcLineCodec
{
public:
    // 解析一行（不含换行）。失败返回 false 并写入 error。
    // P1 约定：含 method 的一律视为通知（stdio 场景下服务端极少主动发请求）。
    static bool parse(const QByteArray& line, JsonRpcLine* out, QString* error);

    // 序列化请求 / 通知，返回值已含尾部 '\n'。
    static QByteArray makeRequest(qint64 id, const QString& method, const QJsonObject& params);
    static QByteArray makeNotification(const QString& method, const QJsonObject& params);
};
```

`src/core/mcp/JsonRpcLineCodec.cpp`：

```cpp
#include "core/mcp/JsonRpcLineCodec.h"

#include <QJsonDocument>

bool JsonRpcLineCodec::parse(const QByteArray& line, JsonRpcLine* out, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("JSON parse error: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        if (error)
            *error = QStringLiteral("unsupported jsonrpc version");
        return false;
    }

    if (obj.value(QStringLiteral("method")).isString()) {
        out->kind = JsonRpcLine::Kind::Notification;
        out->method = obj.value(QStringLiteral("method")).toString();
        out->params = obj.value(QStringLiteral("params"));
        return true;
    }

    if (!obj.contains(QStringLiteral("id"))) {
        if (error)
            *error = QStringLiteral("line has neither method nor id");
        return false;
    }

    out->id = obj.value(QStringLiteral("id")).toVariant().toLongLong();
    if (obj.value(QStringLiteral("error")).isObject()) {
        out->kind = JsonRpcLine::Kind::ErrorResponse;
        out->error = obj.value(QStringLiteral("error")).toObject();
    } else {
        out->kind = JsonRpcLine::Kind::Response;
        out->result = obj.value(QStringLiteral("result"));
    }
    return true;
}

QByteArray JsonRpcLineCodec::makeRequest(qint64 id, const QString& method,
                                         const QJsonObject& params)
{
    QJsonObject obj{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty())
        obj.insert(QStringLiteral("params"), params);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray JsonRpcLineCodec::makeNotification(const QString& method,
                                              const QJsonObject& params)
{
    QJsonObject obj{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty())
        obj.insert(QStringLiteral("params"), params);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}
```

- [ ] **Step 4: 在 CMakeLists.txt 中添加 core 库与测试 target**

在 `enable_testing()` 之前插入：

```cmake
add_library(synthforge_core STATIC
    src/core/mcp/JsonRpcLineCodec.cpp
)
target_include_directories(synthforge_core PUBLIC src)
target_link_libraries(synthforge_core PUBLIC Qt6::Core)
```

在文件末尾追加：

```cmake
add_executable(tst_jsonrpc tests/tst_jsonrpc.cpp)
target_link_libraries(tst_jsonrpc PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_jsonrpc COMMAND tst_jsonrpc)
```

- [ ] **Step 5: 重新配置、构建并运行测试**

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `tst_smoke` 与 `tst_jsonrpc` 全部 Passed（`tst_jsonrpc` 8 个用例：响应/错误/通知/数组 params/错误版本/畸形 JSON/请求序列化/通知无 id）。

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core/mcp/JsonRpcLineCodec.h src/core/mcp/JsonRpcLineCodec.cpp tests/tst_jsonrpc.cpp
git commit -m "feat: 实现 JSON-RPC 行编解码（响应/错误/通知解析与请求序列化）"
```

---

### Task 3: McpClient（握手 + 请求-响应匹配 + 超时）

**Files:**
- Create: `src/core/mcp/McpTransport.h`
- Create: `src/core/mcp/McpClient.h`
- Create: `src/core/mcp/McpClient.cpp`
- Create: `tests/tst_mcpclient.cpp`
- Modify: `CMakeLists.txt`（core 库追加 `McpClient.cpp`；新增 `tst_mcpclient` target）

**Interfaces:**
- Consumes: `JsonRpcLineCodec`（Task 2）。
- Produces:
  - `class McpTransport : QObject` — 纯虚接口 `start() / sendLine(QByteArray) / stop()`；信号 `started()`、`lineReceived(const QByteArray&)`（不含换行）、`stderrChunk(const QByteArray&)`、`stopped(int exitCode)`。
  - `class McpClient : QObject` — 构造 `McpClient(McpTransport* transport, QObject* parent = nullptr)`；方法 `start()`、`isReady()`、`setRequestTimeout(int ms)`（默认 30000）、`listTools(std::function<void(const QJsonArray&)>, ReplyCallback onErr = nullptr)`、`callTool(QString name, QJsonObject arguments, ReplyCallback onDone)`；信号 `ready(const QJsonObject& serverInfo)`、`failed(const QString& reason)`；`using ReplyCallback = std::function<void(const QJsonValue& result, const QJsonObject& error)>`（error 为空对象表示成功）。
  - 请求 id 从 1 起自增；超时错误码 `-32000`，传输断开的兜底错误码 `-32001`。

- [ ] **Step 1: 写失败的测试（含 FakeTransport 测试替身）**

`tests/tst_mcpclient.cpp`：

```cpp
#include <QtTest>
#include "core/mcp/McpClient.h"
#include "core/mcp/McpTransport.h"

// 内存传输替身：不发真实进程，手动喂入服务端行数据。
class FakeTransport : public McpTransport
{
public:
    void start() override { emit started(); }
    void sendLine(const QByteArray& line) override { sent.append(line); }
    void stop() override { emit stopped(0); }

    QList<QByteArray> sent;

    void feed(const QByteArray& json) { emit lineReceived(json); }
};

class McpClientTest : public QObject
{
    Q_OBJECT

private slots:
    void handshakeSucceeds()
    {
        FakeTransport transport;
        McpClient client(&transport);
        QSignalSpy readySpy(&client, &McpClient::ready);

        client.start();

        QCOMPARE(transport.sent.size(), 1);
        QVERIFY(transport.sent.first().contains("initialize"));
        QVERIFY(transport.sent.first().contains("2025-06-18"));

        transport.feed(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","serverInfo":{"name":"mock","version":"0.0.1"},"capabilities":{}}})");

        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(transport.sent.size(), 2);
        QVERIFY(transport.sent.at(1).contains("notifications/initialized"));
        QVERIFY(client.isReady());
    }

    void initializeErrorEmitsFailed()
    {
        FakeTransport transport;
        McpClient client(&transport);
        QSignalSpy failedSpy(&client, &McpClient::failed);

        client.start();
        transport.feed(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"bad params"}})");

        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(!client.isReady());
    }

    void outOfOrderResponsesMatchById()
    {
        FakeTransport transport;
        McpClient client(&transport);

        QJsonArray toolsResult;
        QJsonValue callResult;
        client.listTools([&](const QJsonArray& tools) { toolsResult = tools; });
        client.callTool(QStringLiteral("echo"),
                        QJsonObject{{"text", "hi"}},
                        [&](const QJsonValue& r, const QJsonObject&) { callResult = r; });

        // 先回第二个请求（id=2），再回第一个（id=1）
        transport.feed(R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"hi"}]}})");
        transport.feed(R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"echo"}]}})");

        QCOMPARE(callResult.toObject().value(QStringLiteral("content")).toArray().size(), 1);
        QCOMPARE(toolsResult.size(), 1);
        QCOMPARE(toolsResult.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("echo"));
    }

    void requestTimeoutDeliversError()
    {
        FakeTransport transport;
        McpClient client(&transport);
        client.setRequestTimeout(30);

        QJsonObject receivedError;
        bool done = false;
        client.callTool(QStringLiteral("slow"), QJsonObject{},
                        [&](const QJsonValue&, const QJsonObject& error) {
                            receivedError = error;
                            done = true;
                        });

        QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
        QCOMPARE(receivedError.value(QStringLiteral("code")).toInt(), -32000);
    }

    void transportStopFailsPending()
    {
        FakeTransport transport;
        McpClient client(&transport);
        client.setRequestTimeout(60000);

        QJsonObject receivedError;
        bool done = false;
        client.callTool(QStringLiteral("x"), QJsonObject{},
                        [&](const QJsonValue&, const QJsonObject& error) {
                            receivedError = error;
                            done = true;
                        });

        transport.stop();
        QVERIFY(done);
        QCOMPARE(receivedError.value(QStringLiteral("code")).toInt(), -32001);
    }
};

QTEST_MAIN(McpClientTest)
#include "tst_mcpclient.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --config Debug --target tst_mcpclient`
Expected: 编译失败（`McpClient.h` 不存在；若提示 target 不存在，先执行 Step 5 的 CMake 注册再重试）。

- [ ] **Step 3: 实现 McpTransport 接口**

`src/core/mcp/McpTransport.h`：

```cpp
#pragma once

#include <QByteArray>
#include <QObject>

// 抽象传输接口：把一行 JSON 送出去，把收到的一行 JSON 抛上来。
// 契约：lineReceived 携带的行内容不含换行；sendLine 的入参已含尾部换行。
class McpTransport : public QObject
{
    Q_OBJECT

public:
    explicit McpTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~McpTransport() override = default;

    virtual void start() = 0;
    virtual void sendLine(const QByteArray& line) = 0;
    virtual void stop() = 0;

signals:
    void started();
    void lineReceived(const QByteArray& line);
    void stderrChunk(const QByteArray& chunk);
    void stopped(int exitCode);
};
```

- [ ] **Step 4: 实现 McpClient**

`src/core/mcp/McpClient.h`：

```cpp
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

class McpTransport;

class McpClient : public QObject
{
    Q_OBJECT

public:
    // error 为空对象表示成功；result 的语义由调用方解释。
    using ReplyCallback = std::function<void(const QJsonValue& result, const QJsonObject& error)>;

    explicit McpClient(McpTransport* transport, QObject* parent = nullptr);

    void start(); // 发起 initialize 握手；成功后自动发送 notifications/initialized
    bool isReady() const { return m_ready; }
    void setRequestTimeout(int ms) { m_timeoutMs = ms; }

    void listTools(std::function<void(const QJsonArray& tools)> onOk,
                   ReplyCallback onErr = nullptr);
    void callTool(const QString& name, const QJsonObject& arguments,
                  ReplyCallback onDone);

signals:
    void ready(const QJsonObject& serverInfo);
    void failed(const QString& reason);

private:
    qint64 sendRequest(const QString& method, const QJsonObject& params, ReplyCallback cb);
    void onLine(const QByteArray& line);
    void resolve(qint64 id, const QJsonValue& result, const QJsonObject& error);
    void failAllPending(const QString& reason);

    McpTransport* m_transport;
    QHash<qint64, ReplyCallback> m_pending;
    qint64 m_lastId = 0;
    int m_timeoutMs = 30000;
    bool m_ready = false;
};
```

`src/core/mcp/McpClient.cpp`：

```cpp
#include "core/mcp/McpClient.h"

#include <QTimer>

#include "core/mcp/JsonRpcLineCodec.h"
#include "core/mcp/McpTransport.h"

McpClient::McpClient(McpTransport* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
    connect(m_transport, &McpTransport::lineReceived, this, &McpClient::onLine);
    connect(m_transport, &McpTransport::stopped, this, [this](int) {
        m_ready = false;
        failAllPending(QStringLiteral("transport stopped"));
    });
}

void McpClient::start()
{
    const QJsonObject params{
        {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
        {QStringLiteral("clientInfo"),
         QJsonObject{{"name", QStringLiteral("SynthForgeStudio")},
                     {"version", QStringLiteral("0.1.0")}}},
        {QStringLiteral("capabilities"), QJsonObject{}}};
    sendRequest(QStringLiteral("initialize"), params,
                [this](const QJsonValue& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        emit failed(QStringLiteral("initialize failed: %1").arg(
                            error.value(QStringLiteral("message")).toString()));
                        return;
                    }
                    m_transport->sendLine(JsonRpcLineCodec::makeNotification(
                        QStringLiteral("notifications/initialized"), QJsonObject{}));
                    m_ready = true;
                    emit ready(result.toObject()
                                   .value(QStringLiteral("serverInfo"))
                                   .toObject());
                });
}

void McpClient::listTools(std::function<void(const QJsonArray&)> onOk, ReplyCallback onErr)
{
    sendRequest(QStringLiteral("tools/list"), QJsonObject{},
                [onOk, onErr](const QJsonValue& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        if (onErr)
                            onErr(QJsonValue(), error);
                        return;
                    }
                    if (onOk)
                        onOk(result.toObject().value(QStringLiteral("tools")).toArray());
                });
}

void McpClient::callTool(const QString& name, const QJsonObject& arguments,
                         ReplyCallback onDone)
{
    const QJsonObject params{{"name", name}, {"arguments", arguments}};
    sendRequest(QStringLiteral("tools/call"), params, std::move(onDone));
}

qint64 McpClient::sendRequest(const QString& method, const QJsonObject& params,
                              ReplyCallback cb)
{
    const qint64 id = ++m_lastId;
    m_pending.insert(id, std::move(cb));
    m_transport->sendLine(JsonRpcLineCodec::makeRequest(id, method, params));

    QTimer::singleShot(m_timeoutMs, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return; // 已被响应或断开处理
        ReplyCallback cb = it.value();
        m_pending.erase(it);
        cb(QJsonValue(), QJsonObject{{"code", -32000},
                                     {"message", QStringLiteral("request timeout")}});
    });
    return id;
}

void McpClient::onLine(const QByteArray& line)
{
    JsonRpcLine msg;
    QString error;
    if (!JsonRpcLineCodec::parse(line, &msg, &error)) {
        emit failed(QStringLiteral("bad line from server: %1").arg(error));
        return;
    }
    if (msg.kind == JsonRpcLine::Kind::Notification)
        return; // P1 忽略服务端通知
    resolve(msg.id, msg.result, msg.error);
}

void McpClient::resolve(qint64 id, const QJsonValue& result, const QJsonObject& error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    ReplyCallback cb = it.value();
    m_pending.erase(it);
    cb(result, error);
}

void McpClient::failAllPending(const QString& reason)
{
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        it.value()(QJsonValue(),
                   QJsonObject{{"code", -32001}, {"message", reason}});
    }
}
```

- [ ] **Step 5: 在 CMakeLists.txt 中注册**

修改 `synthforge_core` 的源文件列表，追加一行：

```cmake
    src/core/mcp/McpClient.cpp
```

在文件末尾追加：

```cmake
add_executable(tst_mcpclient tests/tst_mcpclient.cpp)
target_link_libraries(tst_mcpclient PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_mcpclient COMMAND tst_mcpclient)
```

- [ ] **Step 6: 重新配置、构建并运行测试**

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `tst_mcpclient` 5 个用例全部 Passed（含超时与断开兜底）。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/core/mcp/McpTransport.h src/core/mcp/McpClient.h src/core/mcp/McpClient.cpp tests/tst_mcpclient.cpp
git commit -m "feat: 实现 MCP 客户端握手、请求匹配与超时处理"
```

---

### Task 4: StdioTransport（QProcess 子进程 + 行缓冲）与 mock server

**Files:**
- Create: `src/core/mcp/StdioTransport.h`
- Create: `src/core/mcp/StdioTransport.cpp`
- Create: `tests/mock/mock_mcp_server.cpp`
- Create: `tests/tst_stdio_transport.cpp`
- Modify: `CMakeLists.txt`（core 库追加 `StdioTransport.cpp`；新增 `mock_mcp_server` 与 `tst_stdio_transport` target）

**Interfaces:**
- Consumes: `McpTransport` 接口（Task 3）。
- Produces:
  - `struct BridgeCommand { QString program; QStringList args; QProcessEnvironment env; };`（定义在 `StdioTransport.h`）
  - `StdioTransport(const BridgeCommand& command, QObject* parent = nullptr)` — 实现 `start / sendLine / stop`；stdout 按 `\n` 切分（跨 chunk 拼接，不丢半行）；stderr 原样经 `stderrChunk` 转发；进程退出/启动失败经 `stopped(exitCode)` 通知（启动失败 exitCode = -1）。
  - `mock_mcp_server.exe` — 测试用 stdio 服务端，支持 `initialize` / `tools/list`（1 个 echo 工具）/ `tools/call`（回显 text 参数）；未知方法回 `-32601`；通知不回复。

- [ ] **Step 1: 写失败的集成测试**

`tests/tst_stdio_transport.cpp`：

```cpp
#include <QtTest>

#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"

class StdioTransportTest : public QObject
{
    Q_OBJECT

private slots:
    void endToEndThroughMockServer()
    {
        BridgeCommand command{QString::fromLocal8Bit(MOCK_SERVER_PATH), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);

        QSignalSpy readySpy(&client, &McpClient::ready);
        connect(&transport, &McpTransport::started, &client, &McpClient::start);

        transport.start();
        QVERIFY2(readySpy.wait(15000), "mock server 未在 15 秒内完成握手");

        QJsonArray tools;
        bool listed = false;
        client.listTools([&](const QJsonArray& t) {
            tools = t;
            listed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(listed, 15000);
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("echo"));

        QString echoed;
        bool called = false;
        client.callTool(QStringLiteral("echo"),
                        QJsonObject{{"text", "hello synthforge"}},
                        [&](const QJsonValue& result, const QJsonObject& error) {
                            if (error.isEmpty()) {
                                echoed = result.toObject()
                                             .value(QStringLiteral("content"))
                                             .toArray()
                                             .first()
                                             .toObject()
                                             .value(QStringLiteral("text"))
                                             .toString();
                            }
                            called = true;
                        });
        QTRY_VERIFY_WITH_TIMEOUT(called, 15000);
        QCOMPARE(echoed, QStringLiteral("hello synthforge"));

        transport.stop();
    }
};

QTEST_MAIN(StdioTransportTest)
#include "tst_stdio_transport.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --config Debug --target tst_stdio_transport`
Expected: 编译失败（`StdioTransport.h` 不存在；若提示 target 不存在，先执行 Step 5 的 CMake 注册再重试）。

- [ ] **Step 3: 实现 StdioTransport**

`src/core/mcp/StdioTransport.h`：

```cpp
#pragma once

#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include "core/mcp/McpTransport.h"

struct BridgeCommand
{
    QString program;
    QStringList args;
    QProcessEnvironment env;
};

// 基于 QProcess 的 stdio 传输：拉起桥接子进程，按行切分 stdout，stderr 原样转发。
class StdioTransport : public McpTransport
{
    Q_OBJECT

public:
    explicit StdioTransport(const BridgeCommand& command, QObject* parent = nullptr);

    void start() override;
    void sendLine(const QByteArray& line) override;
    void stop() override;

private:
    void onReadyReadStdout();
    void onReadyReadStderr();

    BridgeCommand m_command;
    QProcess m_process;
    QByteArray m_buffer; // 跨 chunk 的行缓冲：readyRead 可能只拿到半行
};
```

`src/core/mcp/StdioTransport.cpp`：

```cpp
#include "core/mcp/StdioTransport.h"

StdioTransport::StdioTransport(const BridgeCommand& command, QObject* parent)
    : McpTransport(parent), m_command(command)
{
    connect(&m_process, &QProcess::started, this, &McpTransport::started);
    connect(&m_process, &QProcess::readyReadStandardOutput, this,
            &StdioTransport::onReadyReadStdout);
    connect(&m_process, &QProcess::readyReadStandardError, this,
            &StdioTransport::onReadyReadStderr);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit stderrChunk(m_process.errorString().toUtf8() + '\n');
        emit stopped(-1);
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) { emit stopped(exitCode); });
}

void StdioTransport::start()
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_command.env.isEmpty())
        m_process.setProcessEnvironment(m_command.env);
    m_process.start(m_command.program, m_command.args);
}

void StdioTransport::sendLine(const QByteArray& line)
{
    m_process.write(line); // line 已含尾部 '\n'
}

void StdioTransport::stop()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_process.terminate();
    if (!m_process.waitForFinished(3000)) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void StdioTransport::onReadyReadStdout()
{
    m_buffer += m_process.readAllStandardOutput();
    int newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (!line.isEmpty())
            emit lineReceived(line);
    }
}

void StdioTransport::onReadyReadStderr()
{
    emit stderrChunk(m_process.readAllStandardError());
}
```

- [ ] **Step 4: 写 mock server（独立可执行）**

`tests/mock/mock_mcp_server.cpp`：

```cpp
#include <iostream>
#include <string>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// 测试用 MCP stdio mock server：initialize / tools/list / tools/call(echo)。
int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        const QJsonObject request =
            QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        const bool isNotification = !request.contains(QStringLiteral("id"));

        QJsonObject response{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), request.value(QStringLiteral("id"))}};

        if (method == QStringLiteral("initialize")) {
            response.insert(
                QStringLiteral("result"),
                QJsonObject{
                    {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
                    {QStringLiteral("capabilities"),
                     QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
                    {QStringLiteral("serverInfo"),
                     QJsonObject{{QStringLiteral("name"), QStringLiteral("mock")},
                                 {QStringLiteral("version"), QStringLiteral("0.0.1")}}}});
        } else if (method == QStringLiteral("tools/list")) {
            response.insert(
                QStringLiteral("result"),
                QJsonObject{
                    {QStringLiteral("tools"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("name"), QStringLiteral("echo")},
                         {QStringLiteral("description"),
                          QStringLiteral("echo back the text argument")},
                         {QStringLiteral("inputSchema"),
                          QJsonObject{{"type", "object"},
                                      {"properties",
                                       QJsonObject{{"text", QJsonObject{{"type", "string"}}}}}}}}}}}});
        } else if (method == QStringLiteral("tools/call")) {
            const QJsonObject arguments = request.value(QStringLiteral("params"))
                                              .toObject()
                                              .value(QStringLiteral("arguments"))
                                              .toObject();
            response.insert(
                QStringLiteral("result"),
                QJsonObject{{"content",
                             QJsonArray{QJsonObject{
                                 {"type", "text"},
                                 {"text", arguments.value(QStringLiteral("text")).toString()}}}}}});
        } else {
            if (isNotification)
                continue;
            response.insert(QStringLiteral("error"),
                            QJsonObject{{"code", -32601},
                                        {"message", "method not found"}});
        }

        if (isNotification)
            continue; // notifications/initialized 等不回复
        std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString()
                  << std::endl;
    }
    return 0;
}
```

- [ ] **Step 5: 在 CMakeLists.txt 中注册并跑测试**

修改 `synthforge_core` 源文件列表，追加：

```cmake
    src/core/mcp/StdioTransport.cpp
```

在文件末尾追加：

```cmake
add_executable(mock_mcp_server tests/mock/mock_mcp_server.cpp)
target_link_libraries(mock_mcp_server PRIVATE Qt6::Core)

add_executable(tst_stdio_transport tests/tst_stdio_transport.cpp)
target_link_libraries(tst_stdio_transport PRIVATE synthforge_core Qt6::Test)
target_compile_definitions(tst_stdio_transport PRIVATE
    MOCK_SERVER_PATH="$<TARGET_FILE:mock_mcp_server>")
add_dependencies(tst_stdio_transport mock_mcp_server)
add_test(NAME tst_stdio_transport COMMAND tst_stdio_transport)
```

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `tst_stdio_transport` Passed（真实子进程完成握手 → 列工具 → 回显）。

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core/mcp/StdioTransport.h src/core/mcp/StdioTransport.cpp tests/mock/mock_mcp_server.cpp tests/tst_stdio_transport.cpp
git commit -m "feat: 实现 QProcess stdio 传输与行缓冲，集成 mock server 端到端测试"
```

---

### Task 5: 默认桥接命令 + 冒烟 CLI + 真实链路验收

**Files:**
- Create: `src/core/mcp/BridgeCommands.h`
- Create: `src/core/mcp/BridgeCommands.cpp`
- Create: `src/cli/main.cpp`
- Create: `tests/tst_bridge_commands.cpp`
- Modify: `CMakeLists.txt`（core 库追加 `BridgeCommands.cpp`；新增 `synthforge_cli` 与 `tst_bridge_commands` target）

**Interfaces:**
- Consumes: `BridgeCommand`（Task 4）、`StdioTransport`（Task 4）、`McpClient`（Task 3）。
- Produces:
  - `BridgeCommand defaultBridgeCommand(const QString& area)` — `"blender"` → `uvx --python 3.11 mcp-for-blender` + env `UV_PYTHON_PREFERENCE=only-managed`；`"unity"` → `uvx --from mcpforunityserver mcp-for-unity --transport stdio`；未知 area 返回 `program` 为空的命令。
  - `synthforge_cli.exe` — 参数：`--connect<blender|unity>`（必填）、`--list-tools`、`--call<tool> --args<json>`（默认 `{}`）、`--timeout<ms>`（默认 30000）。工具清单/调用结果以 JSON 打到 stdout；日志与错误打到 stderr；退出码 0=成功、1=运行错误、2=参数错误。

- [ ] **Step 1: 写失败的命令映射测试**

`tests/tst_bridge_commands.cpp`：

```cpp
#include <QtTest>

#include "core/mcp/BridgeCommands.h"

class BridgeCommandsTest : public QObject
{
    Q_OBJECT

private slots:
    void blenderDefaults()
    {
        const BridgeCommand cmd = defaultBridgeCommand(QStringLiteral("blender"));
        QCOMPARE(cmd.program, QStringLiteral("uvx"));
        QCOMPARE(cmd.args, (QStringList{QStringLiteral("--python"), QStringLiteral("3.11"),
                                        QStringLiteral("mcp-for-blender")}));
        QCOMPARE(cmd.env.value(QStringLiteral("UV_PYTHON_PREFERENCE")),
                 QStringLiteral("only-managed"));
    }

    void unityDefaults()
    {
        const BridgeCommand cmd = defaultBridgeCommand(QStringLiteral("unity"));
        QCOMPARE(cmd.program, QStringLiteral("uvx"));
        QCOMPARE(cmd.args,
                 (QStringList{QStringLiteral("--from"), QStringLiteral("mcpforunityserver"),
                              QStringLiteral("mcp-for-unity"), QStringLiteral("--transport"),
                              QStringLiteral("stdio")}));
    }

    void unknownAreaReturnsEmptyProgram()
    {
        QVERIFY(defaultBridgeCommand(QStringLiteral("photoshop")).program.isEmpty());
    }
};

QTEST_MAIN(BridgeCommandsTest)
#include "tst_bridge_commands.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --config Debug --target tst_bridge_commands`
Expected: 编译失败（`BridgeCommands.h` 不存在；若提示 target 不存在，先执行 Step 4 的 CMake 注册再重试）。

- [ ] **Step 3: 实现默认命令映射**

`src/core/mcp/BridgeCommands.h`：

```cpp
#pragma once

#include <QString>

#include "core/mcp/StdioTransport.h" // BridgeCommand

// area: "blender" 或 "unity"；未知 area 返回 program 为空的命令。
BridgeCommand defaultBridgeCommand(const QString& area);
```

`src/core/mcp/BridgeCommands.cpp`：

```cpp
#include "core/mcp/BridgeCommands.h"

BridgeCommand defaultBridgeCommand(const QString& area)
{
    BridgeCommand command;
    command.env = QProcessEnvironment::systemEnvironment();

    if (area == QStringLiteral("blender")) {
        command.program = QStringLiteral("uvx");
        command.args = {QStringLiteral("--python"), QStringLiteral("3.11"),
                        QStringLiteral("mcp-for-blender")};
        command.env.insert(QStringLiteral("UV_PYTHON_PREFERENCE"),
                           QStringLiteral("only-managed"));
    } else if (area == QStringLiteral("unity")) {
        command.program = QStringLiteral("uvx");
        command.args = {QStringLiteral("--from"), QStringLiteral("mcpforunityserver"),
                        QStringLiteral("mcp-for-unity"), QStringLiteral("--transport"),
                        QStringLiteral("stdio")};
    }
    return command;
}
```

- [ ] **Step 4: CMake 注册并运行单元测试**

修改 `synthforge_core` 源文件列表，追加：

```cmake
    src/core/mcp/BridgeCommands.cpp
```

在文件末尾追加：

```cmake
add_executable(tst_bridge_commands tests/tst_bridge_commands.cpp)
target_link_libraries(tst_bridge_commands PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_bridge_commands COMMAND tst_bridge_commands)
```

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: `tst_bridge_commands` 3 个用例 Passed。

- [ ] **Step 5: 实现冒烟 CLI**

`src/cli/main.cpp`：

```cpp
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include "core/mcp/BridgeCommands.h"
#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("synthforge-cli"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("SynthForge MCP 冒烟 CLI：连接 Blender / Unity 桥接并列工具或调用工具"));
    parser.addHelpOption();
    parser.addOption({{QStringLiteral("c"), QStringLiteral("connect")},
                      QStringLiteral("连接目标：blender 或 unity"), QStringLiteral("area")});
    parser.addOption({QStringLiteral("list-tools"), QStringLiteral("列出服务端全部工具后退出")});
    parser.addOption({QStringLiteral("call"), QStringLiteral("调用指定工具并打印结果"),
                      QStringLiteral("tool")});
    parser.addOption({QStringLiteral("args"),
                      QStringLiteral("工具参数 JSON 对象"), QStringLiteral("json"),
                      QStringLiteral("{}")});
    parser.addOption({QStringLiteral("timeout"),
                      QStringLiteral("单请求超时毫秒"), QStringLiteral("ms"),
                      QStringLiteral("30000")});
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const BridgeCommand command = defaultBridgeCommand(parser.value(QStringLiteral("connect")));
    if (command.program.isEmpty()) {
        err << "error: --connect 必须为 blender 或 unity\n";
        return 2;
    }

    auto* transport = new StdioTransport(command, &app);
    auto* client = new McpClient(transport, &app);
    client->setRequestTimeout(parser.value(QStringLiteral("timeout")).toInt());

    QObject::connect(transport, &McpTransport::stderrChunk, &app,
                     [&err](const QByteArray& chunk) {
                         err << QString::fromUtf8(chunk);
                         err.flush();
                     });
    QObject::connect(client, &McpClient::failed, &app, [&err](const QString& reason) {
        err << "error: " << reason << '\n';
        err.flush();
        QCoreApplication::exit(1);
    });
    QObject::connect(client, &McpClient::ready, &app, [&](const QJsonObject& serverInfo) {
        err << "connected: " << serverInfo.value(QStringLiteral("name")).toString() << ' '
            << serverInfo.value(QStringLiteral("version")).toString() << '\n';
        err.flush();

        if (parser.isSet(QStringLiteral("call"))) {
            const QJsonObject arguments =
                QJsonDocument::fromJson(parser.value(QStringLiteral("args")).toUtf8())
                    .object();
            client->callTool(parser.value(QStringLiteral("call")), arguments,
                             [&](const QJsonValue& result, const QJsonObject& error) {
                                 if (!error.isEmpty()) {
                                     err << "tool error: "
                                         << QJsonDocument(error).toJson(QJsonDocument::Compact)
                                         << '\n';
                                     err.flush();
                                     QCoreApplication::exit(1);
                                     return;
                                 }
                                 if (result.isObject())
                                     out << QJsonDocument(result.toObject())
                                                .toJson(QJsonDocument::Indented);
                                 else if (result.isArray())
                                     out << QJsonDocument(result.toArray())
                                                .toJson(QJsonDocument::Indented);
                                 out.flush();
                                 QCoreApplication::exit(0);
                             });
        } else {
            client->listTools([&](const QJsonArray& tools) {
                out << QJsonDocument(tools).toJson(QJsonDocument::Indented);
                out.flush();
                QCoreApplication::exit(0);
            });
        }
    });
    QObject::connect(transport, &McpTransport::stopped, &app, [&](int exitCode) {
        err << "bridge exited with code " << exitCode << '\n';
        err.flush();
        QCoreApplication::exit(1);
    });
    QObject::connect(transport, &McpTransport::started, client, &McpClient::start);
    transport->start();
    return app.exec();
}
```

- [ ] **Step 6: CMake 注册 CLI 并构建**

在文件末尾追加：

```cmake
add_executable(synthforge_cli src/cli/main.cpp)
target_link_libraries(synthforge_cli PRIVATE synthforge_core)
```

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Expected: 产出 `build/Debug/synthforge_cli.exe`，全部测试仍绿。

- [ ] **Step 7: 真实链路手工验收**

前置：Blender 已打开且 MCP 插件在运行；Unity 编辑器已打开含 MCP for Unity 包的工程。

1. Blender 列工具：
   Run: `build\Debug\synthforge_cli.exe --connect blender --list-tools`
   Expected: stderr 出现 `connected: ...`；stdout 输出 JSON 数组，含 `execute_blender_code`、`look` 等约 9 个工具；退出码 0。
2. Unity 列工具：
   Run: `build\Debug\synthforge_cli.exe --connect unity --list-tools`
   Expected: stdout 数组约 48 个工具；退出码 0。
3. Blender 工具调用（PowerShell 引号安全写法——外壳用单引号包裹 JSON）：
   Run: `build\Debug\synthforge_cli.exe --connect blender --call get_scene_info --args '{}'`
   Expected: stdout 输出场景信息 JSON。
4. 失败路径（关闭 Blender 或插件后执行）：
   Run: `build\Debug\synthforge_cli.exe --connect blender --list-tools`
   Expected: 不挂起；stderr 出现桥接错误日志；退出码 1。
5. 参数错误：
   Run: `build\Debug\synthforge_cli.exe --connect photoshop --list-tools`
   Expected: stderr `error: --connect 必须为 blender 或 unity`；退出码 2。

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/core/mcp/BridgeCommands.h src/core/mcp/BridgeCommands.cpp src/cli/main.cpp tests/tst_bridge_commands.cpp
git commit -m "feat: 默认桥接命令表与 MCP 冒烟 CLI（真实 Blender/Unity 链路验收）"
```

---

### Task 6: 心跳检测与断线自动重连

**Files:**
- Modify: `src/core/mcp/McpClient.h`
- Modify: `src/core/mcp/McpClient.cpp`
- Modify: `src/core/mcp/StdioTransport.cpp`（`start()` 先清空行缓冲）
- Modify: `tests/mock/mock_mcp_server.cpp`（故障注入：收到 ping 不回复）
- Create: `tests/tst_reconnect.cpp`
- Modify: `CMakeLists.txt`（新增 `tst_reconnect` target）

**Interfaces:**
- Consumes: `McpClient`（Task 3）、`StdioTransport`（Task 4）、`mock_mcp_server`（Task 4）。
- Produces:
  - `void setHeartbeatInterval(int ms)` — 心跳周期，默认 30000；`<= 0` 关闭心跳。
  - `void setHeartbeatTimeout(int ms)` — 单次心跳响应超时，默认 5000；超时即判定连接死亡。
  - `void shutdown()` — 主动停止：关心跳、停 transport、不触发自动重连；程序退出使用。
  - `void prepareRestart()` — `bridge.restart` 用：解除 shutdown、清退避计数（但不起传输）；调用方随后 `transport->start()` 重新握手，启动失败仍能继续自动重连。
  - 信号 `reconnecting(int attempt)`（attempt 从 1 起）与 `reconnected()`（仅断线后的重新握手成功才发射，首次 ready 不发射）。
  - 重连退避 1s / 2s / 4s / 8s / 16s / 30s 封顶后保持 30s，无限重试；仅对「曾经握手成功」的连接生效（首次启动失败不自动重试，顶栏保持灰灯，发送时由前置检查报 `blender-not-ready` / `unity-not-ready`）。
  - `P7` 的 `conn.status` 由这两个信号驱动：`reconnecting` → 黄灯，`ready`/`reconnected` → 绿灯。

- [ ] **Step 1: 写失败的测试**

`tests/tst_reconnect.cpp`：

```cpp
#include <QtTest>

#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"

class ReconnectTest : public QObject
{
    Q_OBJECT

private slots:
    void crashedBridgeIsRestartedAndRehandshaked()
    {
        BridgeCommand command{QString::fromLocal8Bit(MOCK_SERVER_PATH), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);
        connect(&transport, &McpTransport::started, &client, &McpClient::start);

        QSignalSpy readySpy(&client, &McpClient::ready);
        transport.start();
        QVERIFY2(readySpy.wait(15000), "首次握手失败");

        QSignalSpy reconnectingSpy(&client, &McpClient::reconnecting);
        QSignalSpy reconnectedSpy(&client, &McpClient::reconnected);

        transport.stop(); // 模拟桥接进程崩溃（McpClient 视角为意外断开）

        QTRY_VERIFY_WITH_TIMEOUT(reconnectingSpy.count() >= 1, 5000);
        QCOMPARE(reconnectingSpy.first().at(0).toInt(), 1);

        QVERIFY2(reconnectedSpy.wait(15000), "崩溃后未自动重连并重新握手");

        QJsonArray tools;
        bool listed = false;
        client.listTools([&](const QJsonArray& t) {
            tools = t;
            listed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(listed, 15000);
        QCOMPARE(tools.size(), 1);

        client.shutdown();
    }

    void stalledBridgeIsDetectedByHeartbeat()
    {
        qputenv("MOCK_IGNORE_PING", "1"); // mock 收到 ping 不回复
        BridgeCommand command{QString::fromLocal8Bit(MOCK_SERVER_PATH), {},
                              QProcessEnvironment::systemEnvironment()};
        StdioTransport transport(command);
        McpClient client(&transport);
        client.setHeartbeatInterval(200);
        client.setHeartbeatTimeout(500);
        connect(&transport, &McpTransport::started, &client, &McpClient::start);

        QSignalSpy readySpy(&client, &McpClient::ready);
        transport.start();
        QVERIFY2(readySpy.wait(15000), "首次握手失败");

        QSignalSpy reconnectingSpy(&client, &McpClient::reconnecting);
        QTRY_VERIFY_WITH_TIMEOUT(reconnectingSpy.count() >= 1, 10000);

        client.shutdown();
        qunsetenv("MOCK_IGNORE_PING");
    }
};

QTEST_MAIN(ReconnectTest)
#include "tst_reconnect.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --config Debug --target tst_reconnect`
Expected: 编译失败（`setHeartbeatInterval`、`shutdown`、`reconnecting` 均不存在；若提示 target 不存在，先执行 Step 6 的 CMake 注册再重试）。

- [ ] **Step 3: 实现心跳与重连**

`src/core/mcp/McpClient.h` 整文件替换为：

```cpp
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class McpTransport;

class McpClient : public QObject
{
    Q_OBJECT

public:
    // error 为空对象表示成功；result 的语义由调用方解释。
    using ReplyCallback = std::function<void(const QJsonValue& result, const QJsonObject& error)>;

    explicit McpClient(McpTransport* transport, QObject* parent = nullptr);

    void start(); // 发起 initialize 握手；成功后自动发送 notifications/initialized
    void shutdown(); // 主动停止：关心跳、停 transport、不触发自动重连
    void prepareRestart(); // bridge.restart：解除 shutdown 与退避计数，随后由调用方启动传输
    bool isReady() const { return m_ready; }
    void setRequestTimeout(int ms) { m_timeoutMs = ms; }
    void setHeartbeatInterval(int ms); // <= 0 关闭心跳；默认 30000
    void setHeartbeatTimeout(int ms) { m_heartbeatTimeoutMs = ms; } // 默认 5000

    void listTools(std::function<void(const QJsonArray& tools)> onOk,
                   ReplyCallback onErr = nullptr);
    void callTool(const QString& name, const QJsonObject& arguments,
                  ReplyCallback onDone);

signals:
    void ready(const QJsonObject& serverInfo);
    void failed(const QString& reason);
    void reconnecting(int attempt); // attempt 从 1 起
    void reconnected();             // 仅断线后的重新握手成功才发射

private:
    qint64 sendRequest(const QString& method, const QJsonObject& params, ReplyCallback cb,
                       int timeoutMs = -1);
    void onLine(const QByteArray& line);
    void resolve(qint64 id, const QJsonValue& result, const QJsonObject& error);
    void failAllPending(const QString& reason);
    void onTransportStopped();
    void startHeartbeat();
    void sendHeartbeat();
    void scheduleReconnect();

    McpTransport* m_transport;
    QHash<qint64, ReplyCallback> m_pending;
    qint64 m_lastId = 0;
    int m_timeoutMs = 30000;
    bool m_ready = false;

    QTimer m_heartbeatTimer;
    int m_heartbeatIntervalMs = 30000;
    int m_heartbeatTimeoutMs = 5000;
    bool m_heartbeatInFlight = false;

    bool m_everReady = false; // 首次握手成功后才允许自动重连
    bool m_shutdown = false;
    bool m_reconnectScheduled = false;
    int m_reconnectAttempt = 0;
};
```

`src/core/mcp/McpClient.cpp` 修改以下函数（其余不变）：

构造函数——接入接入点与心跳定时器：

```cpp
McpClient::McpClient(McpTransport* transport, QObject* parent)
    : QObject(parent), m_transport(transport)
{
    connect(m_transport, &McpTransport::lineReceived, this, &McpClient::onLine);
    connect(m_transport, &McpTransport::stopped, this,
            [this](int) { onTransportStopped(); });
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &McpClient::sendHeartbeat);
}
```

`start()` 与 `shutdown()`——握手成功后启动心跳，断了重连上发 `reconnected`：

```cpp
void McpClient::start()
{
    m_shutdown = false;
    const QJsonObject params{
        {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
        {QStringLiteral("clientInfo"),
         QJsonObject{{"name", QStringLiteral("SynthForgeStudio")},
                     {"version", QStringLiteral("0.1.0")}}},
        {QStringLiteral("capabilities"), QJsonObject{}}};
    sendRequest(QStringLiteral("initialize"), params,
                [this](const QJsonValue& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        emit failed(QStringLiteral("initialize failed: %1").arg(
                            error.value(QStringLiteral("message")).toString()));
                        if (m_everReady) { // 崩后重启的桥接尚未就绪：继续退避重试
                            m_transport->stop();
                            scheduleReconnect();
                        }
                        return;
                    }
                    m_transport->sendLine(JsonRpcLineCodec::makeNotification(
                        QStringLiteral("notifications/initialized"), QJsonObject{}));
                    m_ready = true;
                    m_reconnectAttempt = 0;
                    startHeartbeat();
                    const QJsonObject serverInfo =
                        result.toObject().value(QStringLiteral("serverInfo")).toObject();
                    if (m_everReady)
                        emit reconnected();
                    m_everReady = true;
                    emit ready(serverInfo);
                });
}

void McpClient::shutdown()
{
    m_shutdown = true;
    m_heartbeatTimer.stop();
    m_ready = false;
    failAllPending(QStringLiteral("client shutdown"));
    m_transport->stop();
}

void McpClient::prepareRestart()
{
    m_shutdown = false;          // 启动失败时仍允许退避重连
    m_reconnectAttempt = 0;
    m_reconnectScheduled = false;
}
```

`sendRequest()` 增加单次超时覆盖（心跳用 5s，普通请求仍用 `m_timeoutMs`）：

```cpp
qint64 McpClient::sendRequest(const QString& method, const QJsonObject& params,
                              ReplyCallback cb, int timeoutMs)
{
    const qint64 id = ++m_lastId;
    m_pending.insert(id, std::move(cb));
    m_transport->sendLine(JsonRpcLineCodec::makeRequest(id, method, params));

    const int timeout = timeoutMs > 0 ? timeoutMs : m_timeoutMs;
    QTimer::singleShot(timeout, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return; // 已被响应或断开处理
        ReplyCallback cb = it.value();
        m_pending.erase(it);
        cb(QJsonValue(), QJsonObject{{"code", -32000},
                                     {"message", QStringLiteral("request timeout")}});
    });
    return id;
}
```

心跳、断线与指数退避——追加在 `failAllPending` 之后：

```cpp
void McpClient::setHeartbeatInterval(int ms)
{
    m_heartbeatIntervalMs = ms;
    if (m_ready)
        startHeartbeat();
}

void McpClient::startHeartbeat()
{
    m_heartbeatTimer.stop();
    if (m_heartbeatIntervalMs > 0)
        m_heartbeatTimer.start(m_heartbeatIntervalMs);
}

void McpClient::sendHeartbeat()
{
    if (!m_ready || m_heartbeatInFlight)
        return;
    m_heartbeatInFlight = true;
    sendRequest(QStringLiteral("ping"), QJsonObject{},
                [this](const QJsonValue&, const QJsonObject& error) {
                    m_heartbeatInFlight = false;
                    if (error.value(QStringLiteral("code")).toInt() != -32000)
                        return; // 有响应（含服务端错误码）说明连接仍活着
                    m_ready = false;
                    m_heartbeatTimer.stop();
                    failAllPending(QStringLiteral("heartbeat timeout"));
                    m_transport->stop(); // 进程卡死时先杀掉，否则 restart 会失败
                    scheduleReconnect();
                },
                m_heartbeatTimeoutMs);
}

void McpClient::onTransportStopped()
{
    const bool wasReady = m_ready;
    m_ready = false;
    m_heartbeatTimer.stop();
    m_heartbeatInFlight = false;
    failAllPending(QStringLiteral("transport stopped"));
    if (m_shutdown)
        return;                          // 主动 shutdown：不重连、不报错
    if (wasReady) {
        emit failed(QStringLiteral("MCP 桥接异常退出，正在自动重连")); // → status off + bridge-crashed 错误卡
        scheduleReconnect();
    } else if (m_everReady) {
        scheduleReconnect();             // 重连/手动 restart 的进程没起来：继续退避，不让灯卡死在“重连中”
    }
}

void McpClient::scheduleReconnect()
{
    if (m_reconnectScheduled || m_shutdown)
        return;
    m_reconnectScheduled = true;
    ++m_reconnectAttempt;
    emit reconnecting(m_reconnectAttempt);
    const int delayMs = qMin(1000 * (1 << qMin(m_reconnectAttempt - 1, 5)), 30000);
    QTimer::singleShot(delayMs, this, [this]() {
        m_reconnectScheduled = false;
        if (m_shutdown || m_ready)
            return; // 已恢复就绪时，陈旧定时器不得再断开健康连接
        m_transport->stop(); // 幂等：NotRunning 时直接返回
        m_transport->start(); // started → 外部接线的 McpClient::start 自动重新握手
    });
}
```

> 崩溃语义：传输意外退出（含心跳超时触发的 `stop()`）先 `emit failed(...)`（顶栏变灰 + P7 `bridge-crashed` 错误卡），再 `scheduleReconnect()` 进入黄灯重连；重连尝试再次失败时只续退避、不重复 `emit failed`；`shutdown()` 主动断开不报错；首次启动时传输就起不来（`m_everReady == false` 的 `stopped` 分支）不 `emit failed`，而首次握手失败仍会 `emit failed`（但不会自动重试）。`bridge.restart` 用 `shutdown()` + `prepareRestart()` 组合：重启后的启动失败仍会退避重试，不会因 shutdown 标志卡死；重连定时器在 `m_ready` 时直接返回，避免陈旧定时器误断健康连接。

- [ ] **Step 4: StdioTransport 支持重启**

`StdioTransport::start()` 开头追加一行，避免崩溃前的半行残留在重连后拼进新响应：

```cpp
void StdioTransport::start()
{
    m_buffer.clear(); // 重连时丢弃上次连接的残留半行
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_command.env.isEmpty())
        m_process.setProcessEnvironment(m_command.env);
    m_process.start(m_command.program, m_command.args);
}
```

- [ ] **Step 5: mock server 故障注入**

`tests/mock/mock_mcp_server.cpp` 在 `if (method == QStringLiteral("initialize")) {` 之前插入一个分支（并把原 `if` 改为 `else if`）：

```cpp
        if (method == QStringLiteral("ping")) {
            if (qEnvironmentVariableIsSet("MOCK_IGNORE_PING"))
                continue; // 故障注入：收到 ping 却不回复，模拟桥接卡死
            response.insert(QStringLiteral("result"), QJsonObject{});
        } else if (method == QStringLiteral("initialize")) {
```

- [ ] **Step 6: CMake 注册并跑测试**

在文件末尾追加：

```cmake
add_executable(tst_reconnect tests/tst_reconnect.cpp)
target_link_libraries(tst_reconnect PRIVATE synthforge_core Qt6::Test)
target_compile_definitions(tst_reconnect PRIVATE
    MOCK_SERVER_PATH="$<TARGET_FILE:mock_mcp_server>")
add_dependencies(tst_reconnect mock_mcp_server)
add_test(NAME tst_reconnect COMMAND tst_reconnect)
```

Run: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<QT_KIT>`
Run: `cmake --build build --config Debug`
Run: `ctest --test-dir build -C Debug -R tst_reconnect --output-on-failure`
Expected: 2 个用例 Passed（崩溃后 1s 重启并重新握手、卡死后 5s 内由心跳判定断线）。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/core/mcp/McpClient.h src/core/mcp/McpClient.cpp src/core/mcp/StdioTransport.cpp tests/mock/mock_mcp_server.cpp tests/tst_reconnect.cpp
git commit -m "feat: MCP 心跳检测与桥接崩溃自动重连（指数退避 + reconnect 信号）"
```

---

## P1 完成标准（Definition of Done）

- [ ] `ctest --test-dir build -C Debug` 全绿：`tst_smoke`、`tst_jsonrpc`、`tst_mcpclient`、`tst_stdio_transport`、`tst_bridge_commands`、`tst_reconnect`
- [ ] 心跳超时与桥接崩溃均能自动重连并恢复工具调用（`tst_reconnect` 2 用例）
- [ ] 真实 Blender 链路：`--connect blender --list-tools` 返回工具清单（约 9 个）
- [ ] 真实 Unity 链路：`--connect unity --list-tools` 返回工具清单（约 48 个）
- [ ] 真实工具调用：`--call get_scene_info` 返回场景 JSON
- [ ] 失败路径优雅：桥接缺失时非零退出码、无挂起、无崩溃
- [ ] stdout 仅协议/结果数据，日志与错误全部在 stderr
