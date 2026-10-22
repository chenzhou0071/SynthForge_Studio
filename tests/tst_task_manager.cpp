#include <QSignalSpy>
#include <QStringList>
#include <QtTest>

#include "core/tasks/TaskManager.h"

class TaskManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void runsSeriallyPerArea()
    {
        TaskManager tm;
        QStringList order;
        auto make = [&](const QString& id) {
            return Task{id, QStringLiteral("blender"), QString(),
                        [&order](const QString& tid, TaskManager* m) {
                            order << tid;
                            m->finish(tid);
                        }};
        };
        tm.enqueue(make(QStringLiteral("t1")));
        tm.enqueue(make(QStringLiteral("t2")));
        QCOMPARE(order, (QStringList{QStringLiteral("t1"), QStringLiteral("t2")}));
    }

    void cancelDropsQueuedAndFlagsRunning()
    {
        TaskManager tm;
        QString runningId;
        tm.enqueue(Task{QStringLiteral("t1"), QStringLiteral("blender"), QString(),
                        [&runningId](const QString& tid, TaskManager*) {
                            runningId = tid; // 不调 finish：模拟异步进行中
                        }});
        tm.enqueue(Task{QStringLiteral("t2"), QStringLiteral("blender"), QString(),
                        [](const QString&, TaskManager*) {}});
        QCOMPARE(runningId, QStringLiteral("t1")); // 第一个任务已启动
        QSignalSpy finishedSpy(&tm, &TaskManager::finished);

        tm.cancelArea(QStringLiteral("blender"));

        QVERIFY(tm.isCancelled(QStringLiteral("t1")));
        QCOMPARE(finishedSpy.count(), 1); // 仅 t2 被丢弃并通知
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
    }

    void areasRunInParallel()
    {
        TaskManager tm;
        QStringList order;
        tm.enqueue(Task{QStringLiteral("a1"), QStringLiteral("blender"), QString(),
                        [&order](const QString&, TaskManager*) {
                            order << QStringLiteral("a1-start");
                        }});
        tm.enqueue(Task{QStringLiteral("u1"), QStringLiteral("unity"), QString(),
                        [&order](const QString&, TaskManager*) {
                            order << QStringLiteral("u1-start");
                        }});
        QCOMPARE(order, (QStringList{QStringLiteral("a1-start"), QStringLiteral("u1-start")}));
    }

    void cancellingRunningTaskReportsCancelledOnFinish()
    {
        TaskManager tm;
        tm.enqueue(Task{QStringLiteral("t1"), QStringLiteral("blender"), QString(),
                        [](const QString&, TaskManager*) {
                            // 模拟异步进行中，稍后才 finish
                        }});
        QSignalSpy finishedSpy(&tm, &TaskManager::finished);

        tm.cancelArea(QStringLiteral("blender"));
        QVERIFY(tm.isCancelled(QStringLiteral("t1")));
        QCOMPARE(finishedSpy.count(), 0); // 运行中任务尚未结束

        tm.finish(QStringLiteral("t1"));
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.first().at(0).toString(), QStringLiteral("t1"));
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
    }

    void duplicateFinishIsIgnored()
    {
        TaskManager tm;
        int finishedCount = 0;
        connect(&tm, &TaskManager::finished, this,
                [&finishedCount](const QString&, bool) { ++finishedCount; });
        tm.enqueue(Task{QStringLiteral("t1"), QStringLiteral("blender"), QString(),
                        [](const QString& tid, TaskManager* m) { m->finish(tid); }});
        QCOMPARE(finishedCount, 1); // run 内部已 finish

        tm.finish(QStringLiteral("t1")); // 重复 finish 不应再发信号
        QCOMPARE(finishedCount, 1);
    }
};

QTEST_MAIN(TaskManagerTest)
#include "tst_task_manager.moc"
