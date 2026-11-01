#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "core/env/EnvChecker.h"

// 环境自检：uvx 决定 Blender 桥能不能拉起来，两个桥的连接数决定顶栏能不能变绿。
// 全部走异步信号，任何一路取不到都必须优雅报 failed，不能卡住也不能崩。
class EnvCheckerTest : public QObject
{
    Q_OBJECT

private slots:
    // uvx 不存在（没装或拼错路径）：必须回 failed，而不是永远等不到结果
    void missingUvxReportsFailureInsteadOfHanging()
    {
        EnvChecker checker(QStringLiteral("definitely-not-a-real-binary-xyz"), nullptr, nullptr);
        QSignalSpy spy(&checker, &EnvChecker::result);
        checker.run();
        QVERIFY(spy.wait(8000));
        const QJsonObject report = spy.first().first().toJsonObject();
        QCOMPARE(report.value(QStringLiteral("uvx")).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));
    }

    // 拿 cmake 当“可用的 uvx”：它能构建本工程，就一定认识 --version
    void availableCommandReportsReadyWithDetail()
    {
        const QString cmake = QStandardPaths::findExecutable(QStringLiteral("cmake"));
        if (cmake.isEmpty())
            QSKIP("本机 PATH 里没有 cmake，跳过成功分支");
        EnvChecker checker(cmake, nullptr, nullptr);
        QSignalSpy spy(&checker, &EnvChecker::result);
        checker.run();
        QVERIFY(spy.wait(8000));
        const QJsonObject uvx =
            spy.first().first().toJsonObject().value(QStringLiteral("uvx")).toObject();
        QCOMPARE(uvx.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
        QVERIFY2(uvx.value(QStringLiteral("detail")).toString().contains(QStringLiteral("version")),
                 qPrintable(uvx.value(QStringLiteral("detail")).toString()));
    }

    // 桥没连上（这里连对象都没有）：两路各自报自己的 reason，不能互相顶替
    void absentRuntimesReportTheirOwnReason()
    {
        EnvChecker checker(QStringLiteral("definitely-not-a-real-binary-xyz"), nullptr, nullptr);
        QSignalSpy spy(&checker, &EnvChecker::result);
        checker.run();
        QVERIFY(spy.wait(8000));
        const QJsonObject report = spy.first().first().toJsonObject();
        QCOMPARE(report.value(QStringLiteral("blender")).toObject()
                     .value(QStringLiteral("detail"))
                     .toString(),
                 QStringLiteral("blender-not-ready"));
        QCOMPARE(report.value(QStringLiteral("unity")).toObject()
                     .value(QStringLiteral("detail"))
                     .toString(),
                 QStringLiteral("unity-not-ready"));
        for (const QString& key : {QStringLiteral("uvx"), QStringLiteral("blender"),
                                   QStringLiteral("unity")})
            QCOMPARE(report.value(key).toObject().value(QStringLiteral("state")).toString(),
                     QStringLiteral("failed")); // 三路都得给结论，缺一路前端就会空一块
    }
};

QTEST_MAIN(EnvCheckerTest)
#include "tst_env_checker.moc"
