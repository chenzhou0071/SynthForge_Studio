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
