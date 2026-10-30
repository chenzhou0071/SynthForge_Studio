#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>
#include <QtTest>

#include "core/coreapp/CoreApp.h"
#include "core/mcp/BridgeCommands.h"
#include "core/store/SessionStore.h"
#include "core/ws/WsServer.h"
#include "mock/MockLlmServer.h"
#include "mock/MockCloud3dServer.h"

namespace {

// 迟到的 SSE 服务端：accept 后挂 400ms，再一次性回完整响应。
// 用 Content-Length 而不是 chunked：网关只关心 body 是合法 SSE。
// 写完不主动断开，这样"连接已断"只可能由 abortTag 造成（而不是服务端收尾）。
class DelayedSseServer : public QObject
{
public:
    DelayedSseServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                const int index = m_sockets.size();
                m_sockets.append(socket);
                m_disconnected.append(false);
                m_requests.append(QByteArray());
                socket->setParent(this);
                connect(socket, &QTcpSocket::disconnected, this,
                        [this, index]() { m_disconnected[index] = true; });
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, index]() {
                    m_requests[index].append(socket->readAll());
                });
                QTimer::singleShot(400, this, [socket]() {
                    if (socket->state() != QAbstractSocket::ConnectedState)
                        return; // 已被 abortTag 掐断，迟到的回复发不出去
                    const QByteArray body =
                        "data: {\"choices\":[{\"delta\":{\"content\":\"迟到的回复\"}}]}\n\n"
                        "data: [DONE]\n\n";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Content-Length: "
                                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n"
                                  + body);
                    socket->flush();
                });
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort());
    }
    int acceptedCount() const { return m_sockets.size(); }
    bool acceptedAt(int index) const { return index < m_sockets.size(); }
    bool disconnectedAt(int index) const
    {
        return index < m_disconnected.size() && m_disconnected.at(index);
    }
    QByteArray rawRequestAt(int index) const
    {
        return index < m_requests.size() ? m_requests.at(index) : QByteArray();
    }

private:
    QTcpServer m_server;
    QList<QPointer<QTcpSocket>> m_sockets;
    QList<bool> m_disconnected;
    QList<QByteArray> m_requests;
};

// WebSocket 测试客户端：请求-响应 + 事件流水。
// QTRY_VERIFY 内部会展开 `return;`，不能放进返回 QJsonObject 的 lambda，
// 所以 request 自己轮询 QSignalSpy；事件另存一份 log 用于判断"清空后有没有迟到广播"。
class WsClient
{
public:
    bool connectTo(CoreApp& app)
    {
        QObject::connect(&m_socket, &QWebSocket::textMessageReceived, &m_socket,
                         [this](const QString& text) {
                             m_log.append(QJsonDocument::fromJson(text.toUtf8()).object());
                         });
        QSignalSpy connected(&m_socket, &QWebSocket::connected);
        m_socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                               .arg(app.ws()->port())
                               .arg(app.ws()->token())));
        return connected.wait(5000);
    }

    QJsonObject request(const QString& method, const QJsonObject& params)
    {
        const int id = m_nextId++;
        QSignalSpy spy(&m_socket, &QWebSocket::textMessageReceived);
        m_socket.sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"id", id}, {"method", method}, {"params", params}})
                .toJson(QJsonDocument::Compact)));

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            if (spy.isEmpty())
                spy.wait(20);
            while (!spy.isEmpty()) {
                const QJsonObject obj =
                    QJsonDocument::fromJson(spy.takeFirst().at(0).toString().toUtf8()).object();
                if (obj.value(QStringLiteral("id")).toInt() == id)
                    return obj;
            }
        }
        return {};
    }

    int mark() const { return m_log.size(); }

    // mark 之后最后一个同名事件的 payload；没有则返回空对象
    QJsonObject lastEvent(const QString& name, int mark = 0) const
    {
        for (int i = m_log.size() - 1; i >= mark; --i) {
            if (m_log.at(i).value(QStringLiteral("event")).toString() == name)
                return m_log.at(i).value(QStringLiteral("data")).toObject();
        }
        return {};
    }

    // mark 之后的事件名（请求-响应帧没有 event 字段，会被跳过）
    QStringList eventsSince(int mark) const
    {
        QStringList names;
        for (int i = mark; i < m_log.size(); ++i) {
            const QJsonObject& frame = m_log.at(i);
            if (frame.contains(QStringLiteral("event")))
                names << frame.value(QStringLiteral("event")).toString();
        }
        return names;
    }

    // 把某类事件里的 text 字段拼起来：验证流式期间实际发到前端的内容
    QString textSince(const QString& name, int mark = 0) const
    {
        QString text;
        for (int i = mark; i < m_log.size(); ++i) {
            const QJsonObject& frame = m_log.at(i);
            if (frame.value(QStringLiteral("event")).toString() != name)
                continue;
            text += frame.value(QStringLiteral("data"))
                        .toObject()
                        .value(QStringLiteral("text"))
                        .toString();
        }
        return text;
    }

private:
    QWebSocket m_socket;
    int m_nextId = 1;
    QList<QJsonObject> m_log;
};

QByteArray jsonBody(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject configureLlm(WsClient& client, const QString& baseUrl)
{
    return client.request(
        QStringLiteral("settings.save"),
        {{"agent",
          QJsonObject{{"primary",
                       QJsonObject{{"provider", "deepseek"},
                                   {"baseUrl", baseUrl},
                                   {"apiKey", "test-key"},
                                   {"model", "deepseek-chat"}}}}}});
}

QJsonObject sendChat(WsClient& client, const QString& text)
{
    return client.request(QStringLiteral("chat.send"), {{"area", "blender"}, {"text", text}});
}

MockLlmServer::Turn textTurn(const QString& text)
{
    return MockLlmServer::Turn{.frames = {MockLlm::textChunk(text), MockLlm::doneChunk()}};
}

// mock server 与测试 exe 同目录；不用 CMake 宏传路径，Windows 反斜杠会被当成转义
QString mockServerProgram()
{
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mock_mcp_server");
#endif
}

BridgeCommand mockBlenderBridge()
{
    BridgeCommand command;
    command.program = mockServerProgram();
    command.env = QProcessEnvironment::systemEnvironment();
    return command;
}

// 低模链路必须带 mode：不传 mode 会落到 P2 的通用流，不会出草稿卡
QJsonObject sendLowpoly(WsClient& client, const QString& text)
{
    return client.request(QStringLiteral("chat.send"),
                          {{QStringLiteral("area"), QStringLiteral("blender")},
                           {QStringLiteral("mode"), QStringLiteral("lowpoly")},
                           {QStringLiteral("text"), text}});
}

// 取第 index 次 LLM 请求的 system 正文：档位断言只关心喂进去的是哪份模板
QString systemPromptAt(const MockLlmServer& server, int index)
{
    return server.requestObjectAt(index)
        .value(QStringLiteral("messages"))
        .toArray()
        .first()
        .toObject()
        .value(QStringLiteral("content"))
        .toString();
}

int messageCount(WsClient& client)
{
    return client.request(QStringLiteral("session.load"), {{"area", "blender"}})
        .value(QStringLiteral("result"))
        .toObject()
        .value(QStringLiteral("messages"))
        .toArray()
        .size();
}

// 额外开一条只读连接查任务终态：WAL 允许同进程多连接并发读
QString taskState(const QString& dataDir, const QString& area = QStringLiteral("blender"))
{
    SessionStore store(dataDir + QStringLiteral("/sessions.db"));
    if (!store.open())
        return {};
    const QJsonArray tasks = store.tasks(area);
    return tasks.isEmpty()
        ? QString()
        : tasks.first().toObject().value(QStringLiteral("state")).toString();
}

} // namespace

class HeadlessWsTest : public QObject
{
    Q_OBJECT

private slots:
    void settingsAndSessionRoundTrip()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject saved = client.request(
            QStringLiteral("settings.save"),
            {{"agent", QJsonObject{{"primary", QJsonObject{{"provider", "deepseek"},
                                                          {"model", "deepseek-chat"}}}}}});
        QVERIFY(saved.contains(QStringLiteral("result")));

        const QJsonObject got = client.request(QStringLiteral("settings.get"), {});
        QCOMPARE(got.value("result").toObject().value("agent").toObject().value("primary")
                     .toObject().value("model").toString(),
                 QStringLiteral("deepseek-chat"));

        // chat.send 先落库 user 消息再检查模型配置：未配置时只报 error.card，不调用 LLM
        const int mark = client.mark();
        const QJsonObject sent = sendChat(client, QStringLiteral("hello"));
        const QJsonObject result = sent.value("result").toObject();
        QCOMPARE(result.value("accepted").toBool(), false);
        QCOMPARE(result.value("reason").toString(), QStringLiteral("agent-not-configured"));
        QCOMPARE(messageCount(client), 1);
        const QStringList events = client.eventsSince(mark);
        QVERIFY(events.contains(QStringLiteral("chat.message")));
        QVERIFY(events.contains(QStringLiteral("error.card")));

        const QJsonObject unknown = client.request(QStringLiteral("no.such.method"), {});
        QVERIFY(unknown.contains("error"));
    }

    // 输入区两个开关改走后端 ui.ini：页面的 localStorage 在 app:// 下重启就丢
    void uiStatePrefsSurviveRestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            CoreApp app({dir.path(), true});
            QVERIFY(app.start());
            WsClient client;
            QVERIFY(client.connectTo(app));

            const QJsonObject defaults = client.request(QStringLiteral("uiState.get"), {})
                                             .value(QStringLiteral("result"))
                                             .toObject();
            QCOMPARE(defaults.value("blenderMode").toString(), QStringLiteral("lowpoly"));
            QCOMPARE(defaults.value("chatMode").toString(), QStringLiteral("exec"));

            const QJsonObject saved = client.request(
                QStringLiteral("uiState.set"),
                {{"blenderMode", "cloud3d"}, {"chatMode", "ask"}});
            QVERIFY(saved.contains(QStringLiteral("result")));

            // 非法值直接报错，不能把界面写成两个按钮都不选中的状态
            const QJsonObject bad =
                client.request(QStringLiteral("uiState.set"), {{"blenderMode", "cloud9"}});
            QVERIFY(bad.contains(QStringLiteral("error")));
        }

        // 重启：新 CoreApp 用同一个 data-dir，仍读回上次的选择
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        const QJsonObject restored = client.request(QStringLiteral("uiState.get"), {})
                                          .value(QStringLiteral("result"))
                                          .toObject();
        QCOMPARE(restored.value("blenderMode").toString(), QStringLiteral("cloud3d"));
        QCOMPARE(restored.value("chatMode").toString(), QStringLiteral("ask"));
    }

    // 产出形态三档跟两个开关一样走 ui.ini；非法值必须报错而不是静默回落，
    // 否则前端以为自己存成功了、下次开机档位变了
    void uiStateCarriesShapeAndRejectsUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject defaults = client.request(QStringLiteral("uiState.get"), {})
                                         .value(QStringLiteral("result"))
                                         .toObject();
        QCOMPARE(defaults.value("shape").toString(), QStringLiteral("diorama"));

        const QJsonObject bad =
            client.request(QStringLiteral("uiState.set"), {{"shape", "cinema"}});
        QVERIFY2(bad.contains(QStringLiteral("error")), qPrintable(QJsonDocument(bad).toJson()));

        QVERIFY(client.request(QStringLiteral("uiState.set"), {{"shape", "prop"}})
                    .contains(QStringLiteral("result")));
        QCOMPARE(client.request(QStringLiteral("uiState.get"), {})
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value("shape")
                     .toString(),
                 QStringLiteral("prop"));
    }

    // 档位要一路落到草稿行：chat.send 按界面档位选模板，
    // 「重新生成」读的是草稿自己的 meta.shape，不是界面此刻的开关
    void lowpolyDraftRemembersItsShapeTier()
    {
        MockLlmServer server({textTurn(QStringLiteral("书桌提示词")),
                              textTurn(QStringLiteral("书桌提示词二版"))});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));
        QVERIFY(client.request(QStringLiteral("uiState.set"), {{"shape", "prop"}})
                    .contains(QStringLiteral("result")));

        QVERIFY(sendLowpoly(client, QStringLiteral("一张书桌，上面有电脑和杯子"))
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value(QStringLiteral("accepted"))
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() == 1, 10000);
        // 「不要底座」只在道具档模板里有：盒式舞台那份是「正方形底座」
        QVERIFY2(systemPromptAt(server, 0).contains(QStringLiteral("不要底座")),
                 "chat.send 没把界面档位传给摘要器");
        QVERIFY(!systemPromptAt(server, 0).contains(QStringLiteral("正方形底座")));

        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("draft.created")).isEmpty(),
                                 10000);
        const qint64 draftId =
            client.lastEvent(QStringLiteral("draft.created")).value("id").toInteger();
        QVERIFY(draftId > 0);
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        QCOMPARE(store.messageById(draftId)
                     .value(QStringLiteral("meta"))
                     .toObject()
                     .value(QStringLiteral("shape"))
                     .toString(),
                 QStringLiteral("prop"));

        // 界面切回默认档：重新生成仍必须按草稿当初的道具档出模板
        QVERIFY(client.request(QStringLiteral("uiState.set"), {{"shape", "diorama"}})
                    .contains(QStringLiteral("result")));
        QVERIFY(client.request(QStringLiteral("draft.regenerate"), {{"id", draftId}})
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value(QStringLiteral("accepted"))
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() == 2, 10000);
        QVERIFY2(systemPromptAt(server, 1).contains(QStringLiteral("不要底座")),
                 "重新生成丢了草稿自己的档位");
        QVERIFY(!systemPromptAt(server, 1).contains(QStringLiteral("正方形底座")));
    }

    // 讨论模式：Blender 未连接也能答；[[EXEC]] 不进正文，草稿成一行 draft
    void askModeSplitsExecDraftAndKeepsCardsOutOfHistory()
    {
        MockLlmServer server(
            {textTurn(QStringLiteral("建议把招牌换成旧铁皮。\n\n[[EXEC]]\n"
                                     "把招牌换成深绿做旧铁皮\n[[/EXEC]]")),
             textTurn(QStringLiteral("明白，先不改。"))});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        const int mark = client.mark();
        const QJsonObject sent =
            client.request(QStringLiteral("chat.send"),
                           {{"area", "blender"},
                            {"mode", "ask"},
                            {"text", QStringLiteral("这个招牌怎么改才好看")}});
        QVERIFY2(sent.value(QStringLiteral("result")).toObject().value("accepted").toBool(),
                 "ask 不应因 Blender 未就绪被拒绝");

        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("draft.created"), mark).isEmpty(), 10000);
        const QJsonObject message = client.lastEvent(QStringLiteral("chat.message"), mark);
        QCOMPARE(message.value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
        QCOMPARE(message.value(QStringLiteral("content")).toString(),
                 QStringLiteral("建议把招牌换成旧铁皮。"));

        const QJsonObject created = client.lastEvent(QStringLiteral("draft.created"), mark);
        QVERIFY(created.value(QStringLiteral("id")).toInteger() > 0);
        QCOMPARE(created.value(QStringLiteral("prompt")).toString(),
                 QStringLiteral("把招牌换成深绿做旧铁皮"));
        QCOMPARE(created.value(QStringLiteral("origin")).toString(),
                 QStringLiteral("discussion"));

        // 流式期间不能把标记和草稿正文发出去，否则用户会先看到 [[EXEC]]
        const QString streamed = client.textSince(QStringLiteral("chat.delta"), mark);
        QVERIFY2(!streamed.contains(QStringLiteral("[[EXEC]]")), qPrintable(streamed));
        QVERIFY2(!streamed.contains(QStringLiteral("[[/EXEC]]")), qPrintable(streamed));
        QVERIFY2(!streamed.contains(QStringLiteral("把招牌换成深绿做旧铁皮")),
                 qPrintable(streamed));
        QVERIFY2(streamed.contains(QStringLiteral("建议把招牌换成旧铁皮。")),
                 qPrintable(streamed));

        // 第二次讨论：上一轮写下的 draft 行不能当对话历史喂给模型
        QVERIFY(client
                    .request(QStringLiteral("chat.send"),
                             {{"area", "blender"},
                              {"mode", "ask"},
                              {"text", QStringLiteral("先不改")}})
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value("accepted")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 2, 10000);
        const QJsonArray history = server.requestObjectAt(1)
                                       .value(QStringLiteral("messages"))
                                       .toArray();
        for (const QJsonValue& value : history) {
            const QString role =
                value.toObject().value(QStringLiteral("role")).toString();
            QVERIFY2(role == QLatin1String("system") || role == QLatin1String("user")
                         || role == QLatin1String("assistant"),
                     qPrintable(QStringLiteral("草稿行混进了讨论历史：%1").arg(role)));
        }
    }

    // 讨论模板按档位选：道具档不得带盒式舞台的「正方形底座」，且边界要在提示词里写死
    void askUsesPropTemplateWhenShapeIsProp()
    {
        MockLlmServer server({textTurn(QStringLiteral("先聊聊尺寸。"))});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        client.request(QStringLiteral("chat.send"),
                       {{QStringLiteral("area"), QStringLiteral("blender")},
                        {QStringLiteral("mode"), QStringLiteral("ask")},
                        {QStringLiteral("shape"), QStringLiteral("prop")},
                        {QStringLiteral("text"), QStringLiteral("做个书桌怎么样？")}});
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 1, 10000);
        const QString sent = systemPromptAt(server, 0);
        QVERIFY2(sent.contains(QStringLiteral("道具")), qPrintable(sent));
        QVERIFY(!sent.contains(QStringLiteral("正方形底座")));
        // 讨论的边界：能给「看」的能力，也必须禁「改」
        QVERIFY(sent.contains(QStringLiteral("绝对不要修改场景")));
        QVERIFY(sent.contains(QStringLiteral("inspect_object")));
    }

    // 没连 Blender：本轮没有只读工具，模板必须明说看不见场景，别让模型装作看过
    void askWithoutBlendTellsModelItCannotLook()
    {
        MockLlmServer server({textTurn(QStringLiteral("只能凭文字聊。"))});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        client.request(QStringLiteral("chat.send"),
                       {{QStringLiteral("area"), QStringLiteral("blender")},
                        {QStringLiteral("mode"), QStringLiteral("ask")},
                        {QStringLiteral("text"), QStringLiteral("这张桌子怎么改")}});
        QTRY_VERIFY_WITH_TIMEOUT(server.requestCount() >= 1, 10000);
        QVERIFY2(systemPromptAt(server, 0).contains(
                     QStringLiteral("本轮无法读取 Blender 实际参数")),
                 qPrintable(systemPromptAt(server, 0)));
    }

    // 重新生成失败也要回一条 draft.failed：前端靠它停转圈、恢复按钮
    void draftRegenerateFailureEmitsFailedEvent()
    {
        MockLlmServer server(
            {textTurn(QStringLiteral("建议改材质。\n[[EXEC]]\n把招牌换成旧铁皮\n[[/EXEC]]")),
             MockLlmServer::Turn{.frames = {}, .status = 500}});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        QVERIFY(client
                    .request(QStringLiteral("chat.send"),
                             {{"area", "blender"},
                              {"mode", "ask"},
                              {"text", QStringLiteral("招牌怎么改")}})
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value("accepted")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("draft.created")).isEmpty(), 10000);
        const qint64 id = client.lastEvent(QStringLiteral("draft.created"))
                              .value(QStringLiteral("id"))
                              .toInteger();

        const int mark = client.mark();
        QVERIFY(client.request(QStringLiteral("draft.regenerate"), {{"id", id}})
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value("accepted")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("draft.failed"), mark).isEmpty(), 10000);
        const QJsonObject failed = client.lastEvent(QStringLiteral("draft.failed"), mark);
        QCOMPARE(failed.value(QStringLiteral("id")).toInteger(), id);
        QVERIFY(!failed.value(QStringLiteral("reason")).toString().isEmpty());
        QVERIFY(client.eventsSince(mark).contains(QStringLiteral("error.card")));
    }

    // 文件树刷新必须重新扫描磁盘：外部写入的文件也要出现在 assets.list
    void refreshScansFilesWrittenOutsideTheApp()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        QFile glb(library + QStringLiteral("/Props/chair.glb"));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const int mark = client.mark();
        const QJsonObject refreshed =
            client.request(QStringLiteral("fs.operate"),
                           {{"area", "blender"}, {"op", "refresh"}, {"path", ""}})
                .value(QStringLiteral("result"))
                .toObject();
        QVERIFY(refreshed.value(QStringLiteral("ok")).toBool());
        QVERIFY(client.eventsSince(mark).contains(QStringLiteral("fileTree.changed")));

        const QJsonArray items = client.request(QStringLiteral("assets.list"),
                                                {{"area", "blender"}})
                                     .value(QStringLiteral("result"))
                                     .toArray();
        bool found = false;
        for (const QJsonValue& value : items) {
            if (value.toObject().value(QStringLiteral("relativePath")).toString()
                == QLatin1String("Props/chair.glb"))
                found = true;
        }
        QVERIFY2(found, "刷新后 assets.list 里没有外部写入的 chair.glb");
    }

    // 云生成的来源要在 assets.list 里带出来：文件树据此标「云 · glb」
    void assetsListReportsCloudSource()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        QFile glb(library + QStringLiteral("/Props/crate.glb"));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        QVERIFY(QDir().mkpath(library + QStringLiteral("/.synthforge")));
        QFile meta(library + QStringLiteral("/.synthforge/cloud-assets.json"));
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(R"({"version":1,"assets":{"Props/crate.glb":{"provider":"tripo"}}})");
        meta.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonArray items = client.request(QStringLiteral("assets.list"),
                                                {{"area", "blender"}})
                                     .value(QStringLiteral("result"))
                                     .toArray();
        bool found = false;
        for (const QJsonValue& value : items) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("relativePath")).toString()
                == QLatin1String("Props/crate.glb")) {
                QCOMPARE(item.value(QStringLiteral("source")).toString(),
                         QStringLiteral("cloud"));
                QCOMPARE(item.value(QStringLiteral("provider")).toString(),
                         QStringLiteral("tripo"));
                found = true;
            }
            // 元数据目录本身不能出现在目录列表里
            QVERIFY(item.value(QStringLiteral("relativePath")).toString()
                    != QLatin1String(".synthforge"));
        }
        QVERIFY(found);
    }

    // 只有 glb 的云资产：讨论时必须说明没有 .blend、只能重新生成
    void askModeMarksAssetsWithoutBlendSource()
    {
        MockLlmServer server({textTurn(QStringLiteral("这个资产没有源文件，只能重新生成。"))});
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir(library).mkpath(QStringLiteral("Props")));
        QFile glb(QDir(library).filePath(QStringLiteral("Props/chair.glb")));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        QVERIFY(client
                    .request(QStringLiteral("chat.send"),
                             {{"area", "blender"},
                              {"mode", "ask"},
                              {"asset", "Props/chair.glb"},
                              {"text", QStringLiteral("这个能怎么改")}})
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value("accepted")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 10000);

        const QString system = server.requestObjectAt(0)
                                   .value(QStringLiteral("messages"))
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toString();
        QVERIFY2(system.contains(QStringLiteral("无 .blend 源文件，只能重新生成")),
                 qPrintable(system));
        QVERIFY2(system.contains(QStringLiteral("Props/chair.glb（无 .blend，只能重新生成）")),
                 qPrintable(system));

        // 收尾：等回复落库后再销毁应用
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("chat.message")).isEmpty(), 10000);
    }

    void newClientReceivesConnStatusSnapshot()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        // 页面晚于桥接加载：快照必须在连接建立时补发，否则顶栏永远灰灯
        client.request(QStringLiteral("assets.list"), {}); // 抽干队列里的帧
        QVERIFY(client.eventsSince(0).contains(QStringLiteral("conn.status")));
    }

    void assetsListScansLibraryFiles()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        QFile glb(library + QStringLiteral("/Props/chair.glb"));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonArray items = client.request(QStringLiteral("assets.list"), {})
                                     .value(QStringLiteral("result"))
                                     .toArray();
        QCOMPARE(items.size(), 2); // Props 目录 + chair.glb
        QJsonObject item;
        bool sawDir = false;
        for (const QJsonValue& value : items) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("kind")).toString() == QLatin1String("dir")) {
                sawDir = entry.value(QStringLiteral("relativePath")).toString()
                         == QLatin1String("Props");
            } else {
                item = entry;
            }
        }
        QVERIFY(sawDir);
        QCOMPARE(item.value(QStringLiteral("relativePath")).toString(),
                 QStringLiteral("Props/chair.glb"));
        QCOMPARE(item.value(QStringLiteral("kind")).toString(), QStringLiteral("glb"));
        QCOMPARE(item.value(QStringLiteral("name")).toString(), QStringLiteral("chair"));
    }

    // 用户新建的空文件夹可能一个模型都没有，也必须出现在 assets.list 里
    void assetsListIncludesEmptyFolders()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/test1")));

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonArray items = client.request(QStringLiteral("assets.list"), {})
                                     .value(QStringLiteral("result"))
                                     .toArray();
        QCOMPARE(items.size(), 1);
        const QJsonObject item = items.first().toObject();
        QCOMPARE(item.value(QStringLiteral("relativePath")).toString(), QStringLiteral("test1"));
        QCOMPARE(item.value(QStringLiteral("kind")).toString(), QStringLiteral("dir"));
    }

    void fileReadServesTextImagesAndModels()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        QFile note(library + QStringLiteral("/Props/notes.json"));
        QVERIFY(note.open(QIODevice::WriteOnly));
        note.write("{\"ok\":true}");
        note.close();
        QFile png(library + QStringLiteral("/Props/shot.png"));
        QVERIFY(png.open(QIODevice::WriteOnly));
        png.write("\x89PNG\r\n\x1a\n");
        png.close();
        QFile glb(library + QStringLiteral("/Props/chair.glb"));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        QFile blend(library + QStringLiteral("/Props/chair.blend"));
        QVERIFY(blend.open(QIODevice::WriteOnly));
        blend.write("BLENDER");
        blend.close();
        QFile fbx(library + QStringLiteral("/Props/table.fbx")); // 没有同名 .blend：只能显示信息卡
        QVERIFY(fbx.open(QIODevice::WriteOnly));
        fbx.write("FBX");
        fbx.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject text = client.request(QStringLiteral("file.read"),
                                                {{"area", "blender"},
                                                 {"path", "Props/notes.json"}})
                                     .value(QStringLiteral("result"))
                                     .toObject();
        QCOMPARE(text.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(text.value(QStringLiteral("kind")).toString(), QStringLiteral("text"));
        QCOMPARE(text.value(QStringLiteral("content")).toString(), QStringLiteral("{\"ok\":true}"));
        QCOMPARE(text.value(QStringLiteral("language")).toString(), QStringLiteral("json"));

        const QJsonObject image = client.request(QStringLiteral("file.read"),
                                                 {{"area", "blender"},
                                                  {"path", "Props/shot.png"}})
                                      .value(QStringLiteral("result"))
                                      .toObject();
        QCOMPARE(image.value(QStringLiteral("kind")).toString(), QStringLiteral("image"));
        QVERIFY(image.value(QStringLiteral("dataUrl"))
                    .toString()
                    .startsWith(QStringLiteral("data:image/png;base64,")));

        // glb 回可直接给 three.js 的地址，并带上同基名 .blend 供修改入口判定
        const QJsonObject model = client.request(QStringLiteral("file.read"),
                                                 {{"area", "blender"},
                                                  {"path", "Props/chair.glb"}})
                                      .value(QStringLiteral("result"))
                                      .toObject();
        QCOMPARE(model.value(QStringLiteral("kind")).toString(), QStringLiteral("model"));
        QVERIFY(model.value(QStringLiteral("dataUrl"))
                    .toString()
                    .startsWith(QStringLiteral("file://")));
        QCOMPARE(model.value(QStringLiteral("blendPath")).toString(),
                 QStringLiteral("Props/chair.blend"));
        // 前端靠 size + modified 判断导出中的 glb 是否已经写完
        QVERIFY(model.value(QStringLiteral("size")).toDouble() > 0);
        QVERIFY(!model.value(QStringLiteral("modified")).toString().isEmpty());

        const QJsonObject blendInfo = client.request(QStringLiteral("file.read"),
                                                     {{"area", "blender"},
                                                      {"path", "Props/chair.blend"}})
                                          .value(QStringLiteral("result"))
                                          .toObject();
        QCOMPARE(blendInfo.value(QStringLiteral("kind")).toString(), QStringLiteral("info"));
        QVERIFY(blendInfo.value(QStringLiteral("size")).toDouble() > 0);
        QCOMPARE(blendInfo.value(QStringLiteral("blendPath")).toString(),
                 QStringLiteral("Props/chair.blend"));

        const QJsonObject fbxInfo = client.request(QStringLiteral("file.read"),
                                                   {{"area", "blender"},
                                                    {"path", "Props/table.fbx"}})
                                        .value(QStringLiteral("result"))
                                        .toObject();
        QCOMPARE(fbxInfo.value(QStringLiteral("kind")).toString(), QStringLiteral("info"));
        QCOMPARE(fbxInfo.value(QStringLiteral("blendPath")).toString(), QString());
    }

    void fileReadRejectsBadPaths()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library));

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject escape = client.request(QStringLiteral("file.read"),
                                                  {{"area", "blender"},
                                                   {"path", "../outside.txt"}})
                                       .value(QStringLiteral("result"))
                                       .toObject();
        QCOMPARE(escape.value(QStringLiteral("error")).toString(),
                 QStringLiteral("invalid-path"));

        const QJsonObject missing = client.request(QStringLiteral("file.read"),
                                                   {{"area", "blender"},
                                                    {"path", "nope.glb"}})
                                        .value(QStringLiteral("result"))
                                        .toObject();
        QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("not-found"));

        const QJsonObject unity = client.request(QStringLiteral("file.read"),
                                                 {{"area", "unity"}, {"path", "a.glb"}})
                                      .value(QStringLiteral("result"))
                                      .toObject();
        QCOMPARE(unity.value(QStringLiteral("error")).toString(),
                 QStringLiteral("area-not-supported"));
    }

    void mkdirFromFolderPickerBroadcastsTreeChange()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const int mark = client.mark();
        const QJsonObject result = client.request(QStringLiteral("folderPick.mkdir"),
                                                  {{"path", ""}, {"name", "Props"}})
                                       .value(QStringLiteral("result"))
                                       .toObject();
        QCOMPARE(result.value(QStringLiteral("ok")).toBool(), true);
        QVERIFY(QDir(library + QStringLiteral("/Props")).exists());
        QVERIFY(client.eventsSince(mark).contains(QStringLiteral("fileTree.changed")));
    }

    // 没有可续跑的执行现场时必须明确拒绝，而不是静默入队或崩溃
    void continueWithoutFailureIsRejected()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library"))});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject result =
            client.request(QStringLiteral("chat.continue"), {{"area", "blender"}})
                .value(QStringLiteral("result"))
                .toObject();
        QCOMPARE(result.value(QStringLiteral("error")).toString(),
                 QStringLiteral("nothing-to-resume"));
    }

    void retryAndClearRoundTrip()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        // 无重试目标：返回错误而不是崩溃
        const QJsonObject none =
            client.request(QStringLiteral("chat.retry"), {{"area", "blender"}});
        QCOMPARE(none.value("result").toObject().value("error").toString(),
                 QStringLiteral("no-task-to-retry"));

        // 端口 1 必然拒绝连接：让任务快速失败，retry 才有东西可重放
        QVERIFY(configureLlm(client, QStringLiteral("http://127.0.0.1:1/v1"))
                    .contains(QStringLiteral("result")));
        const QJsonObject first = sendChat(client, QStringLiteral("hello"));
        const QString firstId = first.value("result").toObject().value("taskId").toString();
        QVERIFY(!firstId.isEmpty());
        QTest::qWait(200); // 等第一次失败收尾

        const QJsonObject retry =
            client.request(QStringLiteral("chat.retry"), {{"area", "blender"}});
        const QString retryId = retry.value("result").toObject().value("taskId").toString();
        QVERIFY(!retryId.isEmpty());
        QVERIFY(retryId != firstId);
        QTest::qWait(200); // 等重试失败

        QCOMPARE(messageCount(client), 1); // 重试不重复写 user 消息

        const QJsonObject cleared =
            client.request(QStringLiteral("chat.clear"), {{"area", "blender"}});
        QCOMPARE(cleared.value("result").toObject().value("ok").toBool(), true);
        QCOMPARE(messageCount(client), 0);
    }

    void cancelKeepsMessagesAndStopsStream()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        DelayedSseServer server;
        QVERIFY(server.listen());
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        const QJsonObject sent = sendChat(client, QStringLiteral("做便利店"));
        QVERIFY(sent.value("result").toObject().value("accepted").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(server.acceptedAt(0), 3000);

        const int mark = client.mark();
        const QJsonObject cancelled =
            client.request(QStringLiteral("chat.cancel"), {{"area", "blender"}});
        QCOMPARE(cancelled.value("result").toObject().value("ok").toBool(), true);
        // 取消必须真的断网络，而不是只打标记
        QTRY_VERIFY_WITH_TIMEOUT(server.disconnectedAt(0), 2000);

        QTest::qWait(700); // 跨过 400ms 的迟到回复

        QCOMPARE(messageCount(client), 1); // 取消保留已有消息（方案 A：丢弃半截）
        QCOMPARE(taskState(dir.path()), QStringLiteral("cancelled"));
        const QStringList events = client.eventsSince(mark);
        QVERIFY(!events.contains(QStringLiteral("chat.message")));
        QVERIFY(!events.contains(QStringLiteral("error.card")));
    }

    void clearDropsLateReply()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        DelayedSseServer server;
        QVERIFY(server.listen());
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        const QJsonObject sent = sendChat(client, QStringLiteral("做便利店"));
        QVERIFY(sent.value("result").toObject().value("accepted").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(server.acceptedAt(0), 3000);

        const int mark = client.mark();
        const QJsonObject cleared =
            client.request(QStringLiteral("chat.clear"), {{"area", "blender"}});
        QCOMPARE(cleared.value("result").toObject().value("ok").toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(server.disconnectedAt(0), 2000);

        QTest::qWait(700); // 迟到的 [DONE] 已经回来过

        QCOMPARE(messageCount(client), 0); // 迟到的 assistant 不能把消息写回来
        QCOMPARE(taskState(dir.path()), QStringLiteral("cancelled"));
        const QStringList events = client.eventsSince(mark);
        QVERIFY(!events.contains(QStringLiteral("chat.message")));
        QVERIFY(!events.contains(QStringLiteral("error.card")));
    }

    void cancelEndsOnlyTargetArea()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        DelayedSseServer server;
        QVERIFY(server.listen());
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        QVERIFY(client.request(QStringLiteral("chat.send"), {{"area", "blender"}, {"text", "a"}})
                    .value("result").toObject().value("accepted").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(server.acceptedCount(), 1, 3000);
        QVERIFY(client.request(QStringLiteral("chat.send"), {{"area", "unity"}, {"text", "b"}})
                    .value("result").toObject().value("accepted").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(server.acceptedCount(), 2, 3000);

        // 只取消 blender：unity 的流必须还活着（abortTag 不是全局 abort）
        QCOMPARE(client.request(QStringLiteral("chat.cancel"), {{"area", "blender"}})
                     .value("result").toObject().value("ok").toBool(),
                 true);
        QTRY_VERIFY_WITH_TIMEOUT(server.disconnectedAt(0), 2000);
        QTest::qWait(300);
        QVERIFY(!server.disconnectedAt(1));

        QCOMPARE(client.request(QStringLiteral("chat.clear"), {{"area", "unity"}})
                     .value("result").toObject().value("ok").toBool(),
                 true);
        QTRY_VERIFY_WITH_TIMEOUT(server.disconnectedAt(1), 2000);

        QTest::qWait(700); // 跨过 400ms 的迟到回复
        QCOMPARE(taskState(dir.path(), QStringLiteral("blender")), QStringLiteral("cancelled"));
        QCOMPARE(taskState(dir.path(), QStringLiteral("unity")), QStringLiteral("cancelled"));
    }

    void sendsOnlyRecentTwentyMessagesAsContext()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            // 预置 25 条旧消息：启动前直接写库，避免真的发起 25 次 chat.send
            SessionStore seed(dir.path() + QStringLiteral("/sessions.db"));
            QVERIFY(seed.open());
            for (int i = 0; i < 25; ++i)
                seed.appendMessage(QStringLiteral("blender"), QStringLiteral("user"),
                                   QStringLiteral("m%1").arg(i));
        }

        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        DelayedSseServer server;
        QVERIFY(server.listen());
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        QVERIFY(sendChat(client, QStringLiteral("最新一句"))
                    .value("result").toObject().value("accepted").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(server.acceptedCount(), 1, 3000);

        // 26 条历史（25 旧 + 本次 user）只取最近 20 条：m6..m24 + 本次
        QTRY_COMPARE_WITH_TIMEOUT(
            static_cast<int>(server.rawRequestAt(0).count(QByteArrayLiteral("\"role\":"))), 20,
            3000);
        const QByteArray raw = server.rawRequestAt(0);
        QVERIFY(raw.contains("m6"));
        QVERIFY(!raw.contains("m5"));
        QVERIFY(raw.contains("最新一句"));
        QVERIFY(!raw.contains("\"ts\":")); // 只发 role/content，不带 id/ts
    }

    // 界面选的档位必须一路走到规划器：chat.send 记住 open → draft.send 按 open 规划 →
    // plan.created 带 shape/zones/motions，前端计划卡才显示得出档位（Task 6 的后端半边）
    void draftSendDrivesPlannerAndPlanCardCarriesShape()
    {
        MockLlmServer server({
            textTurn(QStringLiteral("主题：轨道站台")),
            textTurn(QStringLiteral(
                R"({"rules":[],"zones":["主轴：轨道沿 +Y 至 120 米","近景：0~20 米 站台"],)"
                R"("motions":["列车进出站"],"groups":[{"title":"路基"},{"title":"接触网"}]})")),
        });
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library")),
                     mockBlenderBridge()});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QTRY_VERIFY_WITH_TIMEOUT(client.lastEvent(QStringLiteral("conn.status"))
                                     .value(QStringLiteral("blender")).toString()
                                 == QStringLiteral("ready"),
                                 10000);
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));
        QVERIFY(client.request(QStringLiteral("uiState.set"), {{"shape", "open"}})
                    .contains(QStringLiteral("result")));

        QVERIFY(sendLowpoly(client, QStringLiteral("一段地铁轨道站台"))
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("accepted")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("draft.created")).isEmpty(),
                                 10000);
        const qint64 draftId = client.lastEvent(QStringLiteral("draft.created"))
                                   .value(QStringLiteral("id")).toInteger();
        QVERIFY(draftId > 0);

        QVERIFY(client.request(QStringLiteral("draft.send"),
                               QJsonObject{{QStringLiteral("id"), draftId},
                                           {QStringLiteral("prompt"),
                                            QStringLiteral("一段地铁轨道站台")}})
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("accepted")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("plan.created")).isEmpty(),
                                 10000);
        const QJsonObject created = client.lastEvent(QStringLiteral("plan.created"));
        // 草稿记住的 open 档要落到 plan.created，且开放场景的 zones/motions 得带出来
        QCOMPARE(created.value(QStringLiteral("shape")).toString(), QStringLiteral("open"));
        QVERIFY2(created.value(QStringLiteral("zones")).toArray().size() >= 1,
                 "plan.created 没带 zones，前端计划卡显示不了空间结构");
        QVERIFY2(created.value(QStringLiteral("motions")).toArray().size() >= 1,
                 "plan.created 没带 motions，前端计划卡显示不了时序动线");
    }

    // file.read 要能回该资产的产出形态：前端据此把三档开关锁到资产原本的档
    void fileReadSurfacesAssetShapeForLocking()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        for (const QString& name : {QStringLiteral("desk.glb"), QStringLiteral("desk.blend")}) {
            QFile f(library + QStringLiteral("/Props/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }
        // 资产卡落库到独立的 assets 表：磁盘 .blend 不带模式，档位只存这里
        {
            SessionStore seed(dir.path() + QStringLiteral("/sessions.db"));
            QVERIFY(seed.open());
            const QJsonObject card{
                {QStringLiteral("name"), QStringLiteral("desk_01")},
                {QStringLiteral("source"), QStringLiteral("lowpoly")},
                {QStringLiteral("shape"), QStringLiteral("prop")},
                {QStringLiteral("files"),
                 QJsonArray{QJsonObject{{QStringLiteral("suffix"), QStringLiteral("glb")},
                                        {QStringLiteral("path"), QStringLiteral("Props/desk.glb")}},
                            QJsonObject{{QStringLiteral("suffix"), QStringLiteral("blend")},
                                       {QStringLiteral("path"), QStringLiteral("Props/desk.blend")}}}}};
            seed.upsertAsset(QStringLiteral("Props/desk.blend"), QStringLiteral("blender"),
                             QStringLiteral("Props/desk.blend"), card);
        }

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject read = client.request(
                                     QStringLiteral("file.read"),
                                     {{QStringLiteral("area"), QStringLiteral("blender")},
                                      {QStringLiteral("path"), QStringLiteral("Props/desk.glb")}})
                                     .value(QStringLiteral("result"))
                                     .toObject();
        QVERIFY2(read.value(QStringLiteral("ok")).toBool(), qPrintable(QJsonDocument(read).toJson()));
        QCOMPARE(read.value(QStringLiteral("blendPath")).toString(),
                 QStringLiteral("Props/desk.blend"));
        QCOMPARE(read.value(QStringLiteral("shape")).toString(), QStringLiteral("prop"));
    }

    // 档位存在 assets 表而非对话消息表：清空本区对话后，仍能回查该资产的模式
    void assetShapeSurvivesChatClear()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        for (const QString& name : {QStringLiteral("desk.glb"), QStringLiteral("desk.blend")}) {
            QFile f(library + QStringLiteral("/Props/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }
        {
            SessionStore seed(dir.path() + QStringLiteral("/sessions.db"));
            QVERIFY(seed.open());
            // 既当历史卡（会被清空删掉），也登记进 assets 表（清空对话不动它）
            const QJsonObject card{
                {QStringLiteral("name"), QStringLiteral("desk_01")},
                {QStringLiteral("source"), QStringLiteral("lowpoly")},
                {QStringLiteral("shape"), QStringLiteral("open")},
                {QStringLiteral("files"), QJsonArray{QJsonObject{
                    {QStringLiteral("suffix"), QStringLiteral("blend")},
                    {QStringLiteral("path"), QStringLiteral("Props/desk.blend")}}}}};
            seed.appendMessage(QStringLiteral("blender"), QStringLiteral("asset"),
                               QString::fromUtf8(
                                   QJsonDocument(card).toJson(QJsonDocument::Compact)),
                               QJsonObject{{QStringLiteral("state"), QStringLiteral("done")}});
            seed.upsertAsset(QStringLiteral("Props/desk.blend"), QStringLiteral("blender"),
                             QStringLiteral("Props/desk.blend"), card);
        }

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        // 清空本区对话：messages 被删，但 assets 表保留
        QVERIFY(client.request(QStringLiteral("chat.clear"),
                               {{QStringLiteral("area"), QStringLiteral("blender")}})
                    .contains(QStringLiteral("result")));

        const QJsonObject read = client.request(
                                     QStringLiteral("file.read"),
                                     {{QStringLiteral("area"), QStringLiteral("blender")},
                                      {QStringLiteral("path"), QStringLiteral("Props/desk.blend")}})
                                     .value(QStringLiteral("result"))
                                     .toObject();
        QVERIFY2(read.value(QStringLiteral("ok")).toBool(), qPrintable(QJsonDocument(read).toJson()));
        QCOMPARE(read.value(QStringLiteral("shape")).toString(), QStringLiteral("open"));
    }

    // 讨论载入资产：无 .blend 的资产（云 glb）只能回错误码，绝不弹「会清空场景」确认
    void sceneLoadRejectsAssetWithoutBlendSource()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        QFile glb(library + QStringLiteral("/Props/cloud_crate.glb"));
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const int mark = client.mark();
        const QJsonObject result = client.request(
                                       QStringLiteral("sceneLoad.request"),
                                       {{QStringLiteral("area"), QStringLiteral("blender")},
                                        {QStringLiteral("asset"), QStringLiteral("Props/cloud_crate.glb")}})
                                       .value(QStringLiteral("result"))
                                       .toObject();
        QCOMPARE(result.value(QStringLiteral("error")).toString(),
                 QStringLiteral("no-blend-source"));
        QVERIFY2(!client.eventsSince(mark).contains(QStringLiteral("sceneLoad.confirm")),
                 "没有源文件绝不能弹「会清空场景」的确认窗");
        QVERIFY2(!client.eventsSince(mark).contains(QStringLiteral("sceneLoad.notice")),
                 "这个入口前端已封死，后端不该再为它做一套提示 UI");
    }

    // 有 .blend：先弹无超时确认；取消后同一 requestId 不能再生效（否则旧弹窗会载入两次）
    void sceneLoadConfirmThenDeclineClearsPendingRequest()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        for (const QString& name : {QStringLiteral("desk.glb"), QStringLiteral("desk.blend")}) {
            QFile f(library + QStringLiteral("/Props/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }

        CoreApp app({dir.path(), true, library, mockBlenderBridge()});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        // request 处理器先查 m_mcp->isReady()：桥接没握手成功只会广播 notice，弹不出 confirm
        QTRY_VERIFY_WITH_TIMEOUT(client.lastEvent(QStringLiteral("conn.status"))
                                     .value(QStringLiteral("blender")).toString()
                                 == QStringLiteral("ready"),
                                 10000);

        const int mark = client.mark();
        client.request(QStringLiteral("sceneLoad.request"),
                       {{QStringLiteral("area"), QStringLiteral("blender")},
                        {QStringLiteral("asset"), QStringLiteral("Props/desk.glb")}});
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("sceneLoad.confirm"), mark).isEmpty(), 5000);
        const QJsonObject confirm = client.lastEvent(QStringLiteral("sceneLoad.confirm"), mark);
        const QString requestId = confirm.value(QStringLiteral("requestId")).toString();
        // file 是基名（弹窗自己拼 .blend 显示），path 才带完整源文件路径
        QCOMPARE(confirm.value(QStringLiteral("file")).toString(), QStringLiteral("desk"));
        QVERIFY(confirm.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("desk.blend")));

        // 取消：弹窗关掉、不载入，讨论仍可正常纯文字进行
        const QJsonObject declined = client.request(
            QStringLiteral("sceneLoad.decline"), {{QStringLiteral("requestId"), requestId}})
            .value(QStringLiteral("result")).toObject();
        QVERIFY(declined.value(QStringLiteral("cancelled")).toBool());
        // 同一个 requestId 不能再被确认：否则旧弹窗点确定会载入两次
        // （handler 自己返回的错误都在 result 里面，外层 error 只由 WsServer::sendError 产生）
        QCOMPARE(client.request(QStringLiteral("sceneLoad.accept"),
                                {{QStringLiteral("requestId"), requestId}})
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("error"))
                     .toString(),
                 QStringLiteral("request-not-found"));
    }

    // 确认载入：走 execute_blender_code 真替换场景 → sceneLoad.loaded + 记档 m_loadedScenes
    void sceneLoadAcceptLoadsAndBroadcastsLoaded()
    {
        QTemporaryDir dir;
        const QString library = dir.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library + QStringLiteral("/Props")));
        for (const QString& name : {QStringLiteral("desk.glb"), QStringLiteral("desk.blend")}) {
            QFile f(library + QStringLiteral("/Props/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }

        CoreApp app({dir.path(), true, library, mockBlenderBridge()});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        QTRY_VERIFY_WITH_TIMEOUT(client.lastEvent(QStringLiteral("conn.status"))
                                     .value(QStringLiteral("blender")).toString()
                                 == QStringLiteral("ready"),
                                 10000);

        const int mark = client.mark();
        client.request(QStringLiteral("sceneLoad.request"),
                       {{QStringLiteral("area"), QStringLiteral("blender")},
                        {QStringLiteral("asset"), QStringLiteral("Props/desk.glb")}});
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("sceneLoad.confirm"), mark).isEmpty(), 5000);
        const QString requestId =
            client.lastEvent(QStringLiteral("sceneLoad.confirm"), mark)
                .value(QStringLiteral("requestId")).toString();

        const QJsonObject accepted = client.request(
            QStringLiteral("sceneLoad.accept"), {{QStringLiteral("requestId"), requestId}})
            .value(QStringLiteral("result")).toObject();
        QVERIFY2(accepted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(accepted).toJson()));
        // 假桥接对 execute_blender_code 回成功：载入完成后广播 loaded 并带基名
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("sceneLoad.loaded"), mark).isEmpty(), 5000);
        QCOMPARE(client.lastEvent(QStringLiteral("sceneLoad.loaded"), mark)
                     .value(QStringLiteral("file")).toString(),
                 QStringLiteral("desk"));
    }

    // 计划卡落库：draft.send 走真实握手 → plan.created 带 id → 刷新能按 id 恢复 → 确认后置 started
    void planCardPersistsAndUpdatesState()
    {
        MockLlmServer server({
            textTurn(QStringLiteral("主题与视角：测试店面")),
            textTurn(QStringLiteral(R"({"rules":[],"groups":[{"title":"底座"},{"title":"主体"}]})")),
        });
        QVERIFY(server.listen());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // 模拟上次会话留下的一张已取消计划卡：它不能混进这次规划请求的历史
        {
            SessionStore seed(dir.path() + QStringLiteral("/sessions.db"));
            QVERIFY(seed.open());
            QVERIFY(seed.appendMessage(QStringLiteral("blender"), QStringLiteral("plan"),
                                       QStringLiteral("{}"),
                                       QJsonObject{{QStringLiteral("state"),
                                                    QStringLiteral("cancelled")}})
                    > 0);
        }

        CoreApp app({dir.path(), true, dir.filePath(QStringLiteral("library")),
                     mockBlenderBridge()});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));
        // 假桥接握手完成后 isReady() 才为真，draft.send 的前置检查才会放行
        QTRY_VERIFY_WITH_TIMEOUT(client.lastEvent(QStringLiteral("conn.status"))
                                     .value(QStringLiteral("blender")).toString()
                                 == QStringLiteral("ready"),
                                 10000);
        QVERIFY(configureLlm(client, server.baseUrl()).contains(QStringLiteral("result")));

        QVERIFY(sendLowpoly(client, QStringLiteral("做一个测试店面"))
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("accepted")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("draft.created")).isEmpty(),
                                 10000);
        const qint64 draftId = client.lastEvent(QStringLiteral("draft.created"))
                                   .value(QStringLiteral("id")).toInteger();
        QVERIFY(draftId > 0);

        const QJsonObject sent =
            client.request(QStringLiteral("draft.send"),
                           QJsonObject{{QStringLiteral("id"), draftId},
                                       {QStringLiteral("prompt"), QStringLiteral("做一个测试店面")}})
                .value(QStringLiteral("result"))
                .toObject();
        QVERIFY2(sent.value(QStringLiteral("accepted")).toBool(),
                 qPrintable(sent.value(QStringLiteral("reason")).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("plan.created")).isEmpty(),
                                 10000);
        const QJsonObject created = client.lastEvent(QStringLiteral("plan.created"));
        const qint64 planId = created.value(QStringLiteral("id")).toInteger();
        QVERIFY(planId > 0);

        // 历史里能看到计划行，且 JSON 能解析
        const QJsonArray rows = client.request(QStringLiteral("session.load"),
                                               {{QStringLiteral("area"), QStringLiteral("blender")}})
                                    .value(QStringLiteral("result")).toObject()
                                    .value(QStringLiteral("messages")).toArray();
        bool found = false;
        for (const QJsonValue& value : rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("id")).toInteger() != planId)
                continue;
            found = true;
            QCOMPARE(row.value(QStringLiteral("role")).toString(), QStringLiteral("plan"));
            QCOMPARE(row.value(QStringLiteral("meta")).toObject()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("pending"));
            QCOMPARE(
                QJsonDocument::fromJson(row.value(QStringLiteral("content")).toString().toUtf8())
                    .object().value(QStringLiteral("groups")).toArray().size(),
                2);
        }
        QVERIFY(found);

        // 第 2 次 LLM 请求是 planner：卡片行（这里是上面种的 plan）不能原样喂给模型
        const QJsonArray sentMessages =
            server.requestObjectAt(1).value(QStringLiteral("messages")).toArray();
        QVERIFY(!sentMessages.isEmpty());
        for (const QJsonValue& value : sentMessages) {
            const QString role = value.toObject().value(QStringLiteral("role")).toString();
            QVERIFY2(role == QLatin1String("system") || role == QLatin1String("user")
                         || role == QLatin1String("assistant"),
                     qPrintable(QStringLiteral("卡片行混进了 LLM 历史：%1").arg(role)));
        }

        // 确认后置为 started，刷新不会又变回可点击
        const QJsonObject confirmed =
            client.request(QStringLiteral("plan.confirm"),
                           QJsonObject{{QStringLiteral("area"), QStringLiteral("blender")},
                                       {QStringLiteral("taskId"),
                                        created.value(QStringLiteral("taskId"))},
                                       {QStringLiteral("id"), planId}})
                .value(QStringLiteral("result"))
                .toObject();
        QCOMPARE(confirmed.value(QStringLiteral("ok")).toBool(), true);
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        QCOMPARE(store.messageById(planId).value(QStringLiteral("meta")).toObject()
                     .value(QStringLiteral("state")).toString(),
                 QStringLiteral("started"));
    }

    // 云模式：草稿 → 选目录 → 轮询 → GLB 落盘；全程不经过 Blender，也不出计划卡
    void cloudModeDraftGoesToFolderPickAndCreatesGlb()
    {
        MockLlmServer llm({textTurn(QStringLiteral("一只木质板条箱，正面有金属扣件。"))});
        QVERIFY(llm.listen());

        MockCloud3dServer cloud;
        QVERIFY(cloud.listen());
        cloud.enqueue("POST", "/task", 200, jsonBody({{"task_id", "c1"}}));
        const QString modelUrl = cloud.baseUrl() + QStringLiteral("/model.glb");
        cloud.enqueue("GET", "/task/c1", 200,
                      jsonBody({{"status", "success"},
                                {"progress", 100},
                                {"model_url", modelUrl}}));
        cloud.enqueue("GET", "/model.glb", 200, QByteArray("glTF", 4));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString library = dir.filePath(QStringLiteral("library"));

        CoreApp app({dir.path(), true, library});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        const QJsonObject saved = client.request(
            QStringLiteral("settings.save"),
            {{QStringLiteral("agent"),
              QJsonObject{{
                  QStringLiteral("primary"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("deepseek")},
                              {QStringLiteral("baseUrl"), llm.baseUrl()},
                              {QStringLiteral("apiKey"), QStringLiteral("test-key")},
                              {QStringLiteral("model"), QStringLiteral("deepseek-chat")}}}}},
             {QStringLiteral("cloud3d"),
              QJsonObject{{QStringLiteral("provider"), QStringLiteral("custom")},
                          {QStringLiteral("custom"),
                           QJsonObject{{QStringLiteral("baseUrl"), cloud.baseUrl()}}}}}});
        QVERIFY(saved.contains(QStringLiteral("result")));

        const int mark = client.mark();
        const QJsonObject sent =
            client.request(QStringLiteral("chat.send"),
                           {{QStringLiteral("area"), QStringLiteral("blender")},
                            {QStringLiteral("mode"), QStringLiteral("cloud3d")},
                            {QStringLiteral("text"), QStringLiteral("做一只木质板条箱")}});
        QVERIFY2(sent.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("accepted")).toBool(),
                 "云模式不应因 Blender 未就绪被拒绝");

        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("draft.created"), mark).isEmpty(),
                                 10000);
        const QJsonObject draft = client.lastEvent(QStringLiteral("draft.created"), mark);
        QCOMPARE(draft.value(QStringLiteral("mode")).toString(), QStringLiteral("cloud3d"));
        const qint64 draftId = draft.value(QStringLiteral("id")).toInteger();
        QVERIFY(draftId > 0);

        const QJsonObject run =
            client.request(QStringLiteral("draft.send"),
                           QJsonObject{{QStringLiteral("id"), draftId},
                                       {QStringLiteral("prompt"),
                                        draft.value(QStringLiteral("prompt")).toString()}})
                .value(QStringLiteral("result"))
                .toObject();
        QVERIFY2(run.value(QStringLiteral("accepted")).toBool(),
                 qPrintable(run.value(QStringLiteral("reason")).toString()));

        QTRY_VERIFY_WITH_TIMEOUT(
            !client.lastEvent(QStringLiteral("folderPick.open"), mark).isEmpty(), 5000);
        QVERIFY(client.lastEvent(QStringLiteral("plan.created"), mark).isEmpty());
        const QString requestId = client.lastEvent(QStringLiteral("folderPick.open"), mark)
                                      .value(QStringLiteral("requestId"))
                                      .toString();

        const QJsonObject picked =
            client.request(QStringLiteral("folderPick.submit"),
                           QJsonObject{{QStringLiteral("requestId"), requestId},
                                       {QStringLiteral("relativePath"), QString()}})
                .value(QStringLiteral("result"))
                .toObject();
        QVERIFY2(picked.value(QStringLiteral("ok")).toBool(),
                 qPrintable(picked.value(QStringLiteral("error")).toString()));

        QTRY_VERIFY_WITH_TIMEOUT(!client.lastEvent(QStringLiteral("asset.created"), mark).isEmpty(),
                                 15000);
        const QJsonObject created = client.lastEvent(QStringLiteral("asset.created"), mark);
        const QString relative = created.value(QStringLiteral("path")).toString();
        QVERIFY(relative.endsWith(QStringLiteral(".glb")));
        QVERIFY(QFile::exists(QDir(library).filePath(relative)));
        QVERIFY(!client.lastEvent(QStringLiteral("task.progress"), mark).isEmpty());

        // 资产产出卡：同一事件带上卡片载荷与落库行号，历史里可恢复
        const qint64 assetId = created.value(QStringLiteral("id")).toInteger();
        QVERIFY(assetId > 0);
        const QJsonObject card = created.value(QStringLiteral("card")).toObject();
        QCOMPARE(card.value(QStringLiteral("source")).toString(),
                 QStringLiteral("cloud3d"));
        QCOMPARE(card.value(QStringLiteral("provider")).toString(), QStringLiteral("custom"));
        const QString summary = card.value(QStringLiteral("prompt")).toString();
        QVERIFY(!summary.isEmpty());
        QVERIFY(summary.size() <= 21); // 约 20 字，超出加省略号
        const QJsonArray files = card.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.at(0).toObject().value(QStringLiteral("suffix")).toString(),
                 QStringLiteral("glb"));
        QVERIFY(!card.contains(QStringLiteral("faces")));

        const QJsonObject page =
            client.request(QStringLiteral("session.load"),
                           QJsonObject{{QStringLiteral("area"), QStringLiteral("blender")},
                                       {QStringLiteral("limit"), 30}})
                .value(QStringLiteral("result"))
                .toObject();
        bool cardRowFound = false;
        for (const QJsonValue& value : page.value(QStringLiteral("messages")).toArray()) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("id")).toInteger() != assetId)
                continue;
            cardRowFound = row.value(QStringLiteral("role")).toString()
                == QLatin1String("asset");
        }
        QVERIFY(cardRowFound);
    }

    void bridgeRestartReportsNotRegistered()
    {
        QTemporaryDir dir;
        CoreApp app({dir.path(), true});
        QVERIFY(app.start());
        WsClient client;
        QVERIFY(client.connectTo(app));

        // blender 已在 P4 注册，这里用与注册表演进无关的未知区名验证 fallback
        const QJsonObject res =
            client.request(QStringLiteral("bridge.restart"), {{"area", "unknown"}});
        QCOMPARE(res.value("result").toObject().value("error").toString(),
                 QStringLiteral("bridge-not-registered"));
    }
};

QTEST_MAIN(HeadlessWsTest)
#include "tst_headless_ws.moc"
