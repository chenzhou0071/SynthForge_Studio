#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <stdexcept>

#include "core/ws/ProtocolRouter.h"

class ProtocolRouterTest : public QObject
{
    Q_OBJECT

private slots:
    void dispatchInvokesRegisteredHandler()
    {
        ProtocolRouter router;
        QString receivedMethod;
        QJsonObject receivedParams;
        router.registerHandler(QStringLiteral("chat.send"),
                               [&](const QJsonObject& params) -> QJsonValue {
                                   receivedMethod = QStringLiteral("chat.send");
                                   receivedParams = params;
                                   return QJsonObject{{QStringLiteral("ok"), true}};
                               });

        const QJsonValue result = router.dispatch(
            QStringLiteral("chat.send"),
            QJsonObject{{QStringLiteral("area"), QStringLiteral("blender")}});

        QCOMPARE(receivedMethod, QStringLiteral("chat.send"));
        QCOMPARE(receivedParams.value(QStringLiteral("area")).toString(),
                 QStringLiteral("blender"));
        QCOMPARE(result.toObject().value(QStringLiteral("ok")).toBool(), true);
    }

    void dispatchUnknownMethodThrows()
    {
        ProtocolRouter router;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 router.dispatch(QStringLiteral("nope"), QJsonObject{}));
    }

    void handlerMayReturnArray()
    {
        ProtocolRouter router;
        router.registerHandler(QStringLiteral("session.list"),
                               [](const QJsonObject&) -> QJsonValue {
                                   return QJsonArray{QJsonObject{{QStringLiteral("id"), 1}},
                                                     QJsonObject{{QStringLiteral("id"), 2}}};
                               });

        const QJsonValue result =
            router.dispatch(QStringLiteral("session.list"), QJsonObject{});

        QVERIFY(result.isArray());
        QCOMPARE(result.toArray().size(), 2);
        QCOMPARE(result.toArray().at(1).toObject().value(QStringLiteral("id")).toInt(), 2);
    }
};

QTEST_MAIN(ProtocolRouterTest)
#include "tst_protocol_router.moc"
