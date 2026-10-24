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
            makeDefaultSettings().value(QStringLiteral("providerBaseUrls")).toObject();
        QCOMPARE(urls.size(), 5);
        QCOMPARE(urls.value(QStringLiteral("doubao")).toString(),
                 QStringLiteral("https://ark.cn-beijing.volces.com/api/v3"));
        QCOMPARE(urls.value(QStringLiteral("deepseek")).toString(),
                 QStringLiteral("https://api.deepseek.com/v1"));
    }

    void dataDirFromArgsUsesExplicitValue()
    {
        const QStringList args{QStringLiteral("synthforge_cli"), QStringLiteral("--headless"),
                               QStringLiteral("--data-dir"), QStringLiteral("E:/tmp/t5")};
        QCOMPARE(dataDirFromArgs(args), QStringLiteral("E:/tmp/t5"));
    }

    void dataDirFromArgsAcceptsEqualsForm()
    {
        const QStringList args{QStringLiteral("synthforge_cli"),
                               QStringLiteral("--data-dir=E:/tmp/t5b")};
        QCOMPARE(dataDirFromArgs(args), QStringLiteral("E:/tmp/t5b"));
    }

    void dataDirFromArgsFallsBackWhenMissing()
    {
        const QStringList args{QStringLiteral("synthforge_cli"), QStringLiteral("--headless")};
        QCOMPARE(dataDirFromArgs(args), defaultDataDir());
    }

    void dataDirFromArgsFallsBackWhenValueEmpty()
    {
        const QStringList dangling{QStringLiteral("synthforge_cli"), QStringLiteral("--data-dir")};
        QCOMPARE(dataDirFromArgs(dangling), defaultDataDir());
        const QStringList empty{QStringLiteral("synthforge_cli"), QStringLiteral("--data-dir=")};
        QCOMPARE(dataDirFromArgs(empty), defaultDataDir());
    }

    void saveLoadRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SettingsStore store(dir.path());
        store.load();

        QJsonObject agent = store.data().value(QStringLiteral("agent")).toObject();
        QJsonObject primary = agent.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("model"), QStringLiteral("gpt-4o-mini"));
        agent.insert(QStringLiteral("primary"), primary);
        QJsonObject root = store.data();
        root.insert(QStringLiteral("agent"), agent);
        store.setData(root);
        QVERIFY(store.save());

        SettingsStore reloaded(dir.path());
        reloaded.load();
        QCOMPARE(reloaded.data()
                     .value(QStringLiteral("agent"))
                     .toObject()
                     .value(QStringLiteral("primary"))
                     .toObject()
                     .value(QStringLiteral("model"))
                     .toString(),
                 QStringLiteral("gpt-4o-mini"));
    }
};

QTEST_MAIN(SettingsStoreTest)
#include "tst_settings_store.moc"
