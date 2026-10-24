#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>
#include <QtTest>

#include "core/coreapp/CoreApp.h"
#include "core/store/SessionStore.h"
#include "core/ws/WsServer.h"

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

private:
    QWebSocket m_socket;
    int m_nextId = 1;
    QList<QJsonObject> m_log;
};

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
