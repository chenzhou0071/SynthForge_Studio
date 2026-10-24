#include <QDir>
#include <QtTest>

#include "core/preflight/PreflightChecker.h"
#include "core/settings/SettingsStore.h"

class PreflightTest : public QObject
{
    Q_OBJECT
private slots:
    void lowpolyFailsWithoutBlenderConnection()
    {
        SettingsStore settings(QDir::tempPath());
        PreflightChecker checker(&settings, nullptr); // nullptr = 未连接
        const auto result = checker.checkLowpoly();
        QVERIFY(!result.ok);
        QCOMPARE(result.reason, QStringLiteral("blender-not-ready"));
    }

    void cloud3dFailsWithoutApiKey()
    {
        SettingsStore settings(QDir::tempPath());
        PreflightChecker checker(&settings, nullptr);
        const auto result = checker.checkCloud3d();
        QVERIFY(!result.ok);
        QCOMPARE(result.reason, QStringLiteral("cloud3d-not-configured"));
    }
};

QTEST_MAIN(PreflightTest)
#include "tst_preflight.moc"
