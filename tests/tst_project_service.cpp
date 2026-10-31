#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include "core/unity/ProjectService.h"

class ProjectServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void validateRequiresAssetsAndProjectSettings()
    {
        QTemporaryDir dir;
        ProjectService service;
        QVERIFY(!service.validate(dir.path()));          // 空目录不合法
        QDir(dir.path()).mkdir("Assets");
        QDir(dir.path()).mkdir("ProjectSettings");
        QVERIFY(service.validate(dir.path()));
    }

    void switchOrderIsDeactivateThenActivate()
    {
        QTemporaryDir dirA, dirB;
        for (auto* d : {&dirA, &dirB}) {
            QDir(d->path()).mkdir("Assets");
            QDir(d->path()).mkdir("ProjectSettings");
        }
        ProjectService service;
        QStringList events;
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) {
                             events << info.path + ":" + info.state;
                         });
        service.setBridgeHooks([] { /* stop */ }, [] { /* start */ },
                               [&](const QString&) { /* cancel tasks */ });

        service.select(dirA.path());
        service.select(dirB.path());

        QCOMPARE(events.size(), 5);
        QCOMPARE(events.at(0), dirA.path() + ":activating");
        QCOMPARE(events.at(1), dirA.path() + ":active");
        QCOMPARE(events.at(2), dirA.path() + ":deactivating");
        QCOMPARE(events.at(3), dirB.path() + ":activating");
        QCOMPARE(events.at(4), dirB.path() + ":active");
    }

    void switchWithRunningTasksRequiresConfirmation()
    {
        QTemporaryDir dirA, dirB;
        for (auto* d : {&dirA, &dirB}) {
            QDir(d->path()).mkdir("Assets");
            QDir(d->path()).mkdir("ProjectSettings");
        }
        ProjectService service;
        service.setBridgeHooks([] {}, [] {}, [](const QString&) {});
        service.setRunningTaskQuery([] { return 2; });
        QStringList states;
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) { states << info.state; });

        service.select(dirA.path());
        int running = 0;
        QCOMPARE(service.requestSwitch(dirB.path(), &running),
                 ProjectService::SwitchResult::NeedsConfirm);
        QCOMPARE(running, 2);
        QCOMPARE(states.size(), 2);                     // 仍停在 A（activating/active）
        QCOMPARE(service.activePath(), dirA.path());

        service.confirmSwitch(dirB.path());
        QCOMPARE(states.size(), 5);
        QCOMPARE(service.activePath(), dirB.path());
    }

    void waitsForUnityThenActivates()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("Assets");
        QDir(dir.path()).mkdir("ProjectSettings");
        ProjectService service;
        bool ready = false;
        service.setBridgeHooks([] {}, [] {}, [](const QString&) {});
        service.setBridgeReadyQuery([&] { return ready; });
        service.setWaitIntervalForTest(10);             // 生产 5000ms × 12 次

        QStringList states;
        QSignalSpy waiting(&service, &ProjectService::waitingUnity);
        QObject::connect(&service, &ProjectService::stateChanged, this,
                         [&](const ProjectInfo& info) { states << info.state; });

        service.select(dir.path());
        QCOMPARE(states, QStringList{QStringLiteral("activating")});
        QTRY_VERIFY(waiting.count() >= 2);              // 未就绪：进入等待并自动重试
        ready = true;
        QTRY_COMPARE(states.size(), 2);
        QCOMPARE(states.last(), QStringLiteral("active"));
    }
};

QTEST_MAIN(ProjectServiceTest)
#include "tst_project_service.moc"
