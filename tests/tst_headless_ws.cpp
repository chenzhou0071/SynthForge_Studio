#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QWebSocket>
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
                             .arg(app.ws()->port())
                             .arg(app.ws()->token())));
        QVERIFY(connected.wait(5000));

        // 请求-响应助手：QTRY_VERIFY 内部会展开 `return;`，不能放进返回 QJsonObject 的 lambda，
        // 这里用 QSignalSpy 轮询指定 id 的响应帧
        int nextId = 1;
        auto request = [&](const QString& method, const QJsonObject& params) {
            QJsonObject frame{{"id", nextId}, {"method", method}, {"params", params}};
            const int expected = nextId++;
            QSignalSpy spy(&socket, &QWebSocket::textMessageReceived);
            socket.sendTextMessage(QString::fromUtf8(
                QJsonDocument(frame).toJson(QJsonDocument::Compact)));

            QJsonObject received;
            QElapsedTimer timer;
            timer.start();
            while (received.isEmpty() && timer.elapsed() < 5000) {
                if (spy.isEmpty())
                    spy.wait(20);
                while (!spy.isEmpty()) {
                    const QList<QVariant> args = spy.takeFirst();
                    const QJsonObject obj =
                        QJsonDocument::fromJson(args.at(0).toString().toUtf8()).object();
                    if (obj.value("id").toInt() == expected)
                        received = obj;
                }
            }
            return received;
        };

        const QJsonObject saved = request(
            "settings.save",
            {{"agent", QJsonObject{{"primary", QJsonObject{{"provider", "deepseek"},
                                                          {"model", "deepseek-chat"}}}}}});
        QVERIFY(saved.contains("result"));

        const QJsonObject got = request("settings.get", {});
        QCOMPARE(got.value("result").toObject().value("agent").toObject().value("primary")
                     .toObject().value("model").toString(),
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
