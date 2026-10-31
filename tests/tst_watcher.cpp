#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "core/unity/DirectoryWatcher.h"

class WatcherTest : public QObject
{
    Q_OBJECT
private slots:
    void ignoresLibraryAndMeta()
    {
        DirectoryWatcher watcher;
        QVERIFY(watcher.isIgnored("/proj/Library/cache.bin"));
        QVERIFY(watcher.isIgnored("/proj/Assets/Player.cs.meta"));
        QVERIFY(watcher.isIgnored("/proj/Temp/xx"));
        QVERIFY(!watcher.isIgnored("/proj/Assets/Player.cs"));
    }

    // 沙箱/CI 下 QFileSystemWatcher 未必投递真实通知，故直接注入变更验证合并逻辑。
    void debouncesBurstIntoOneEvent()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        for (int i = 0; i < 5; ++i)
            watcher.noteChange(dir.path());                 // 模拟 5 次目录变更信号
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);    // 400ms 防抖合并成 1 次
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("incremental"));
        QCOMPARE(spy.first().at(1).toStringList(), QStringList{QStringLiteral(".")});
    }

    void suppressWindowSwallowsEvents()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        watcher.beginSuppress();
        watcher.noteChange(dir.path());                     // 抑制窗口内的变更应被丢弃
        QTest::qWait(600);                                  // 越过 400ms 防抖窗口
        watcher.endSuppress();
        QTest::qWait(800);                                  // 500ms 落盘宽限期结束
        QCOMPARE(spy.count(), 0);
    }

    // 单批变更 >200 条降级为一次全量刷新（规格 12 风险 6：watcher 溢出）
    void manyChangesDegradeToFull()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());

        for (int i = 0; i < 201; ++i)
            watcher.noteChange(dir.path() + QString("/d%1").arg(i));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("full"));
    }

    // 切换工程后 stop()：旧 pending 全丢、监控移除，之后的变更不再产生事件
    void stopDropsPendingAndWatching()
    {
        QTemporaryDir dir;
        DirectoryWatcher watcher;
        QSignalSpy spy(&watcher, &DirectoryWatcher::treeChanged);
        watcher.watch(dir.path());
        watcher.noteChange(dir.path());                     // 先攒一条 pending
        watcher.stop();                                     // 立即丢弃
        QTest::qWait(600);
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(WatcherTest)
#include "tst_watcher.moc"
