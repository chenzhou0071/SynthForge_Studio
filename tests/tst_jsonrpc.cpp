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
