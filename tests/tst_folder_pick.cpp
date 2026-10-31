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
#include "core/ws/WsServer.h"

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
