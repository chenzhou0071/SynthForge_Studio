#include <QtTest>

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <functional>

#include "core/logging/LogService.h"

// 日志层是全局 handler，每条用例用完必须 shutdown，否则会漏到下一条用例的目录里。
class LogServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { LogService::shutdown(); }

    // 落盘行格式：[HH:mm:ss.zzz] [W] category: message
    void writesFormattedLineToDatedFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LogService::install(dir.filePath(QStringLiteral("logs")));
        qWarning() << QStringLiteral("桥接断了");

        const QString path = LogService::currentLogFile();
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QVERIFY2(path.contains(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))),
                 qPrintable(path)); // 文件名按天

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY2(QRegularExpression(QStringLiteral(R"(\[\d{2}:\d{2}:\d{2}\.\d{3}\])"))
                     .match(text)
                     .hasMatch(),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("] [W] ")), qPrintable(text)); // 级别标记
        QVERIFY2(text.contains(QStringLiteral("桥接断了")), qPrintable(text));
    }

    // 轮转命名：当天首文件无序号，之后 -1 -2
    void filePathForAppendsRotateSuffix()
    {
        const QDate day(2026, 11, 1);
        QCOMPARE(LogService::filePathFor(QStringLiteral("logs"), day, 0),
                 QStringLiteral("logs/2026-11-01.log"));
        QCOMPARE(LogService::filePathFor(QStringLiteral("logs"), day, 2),
                 QStringLiteral("logs/2026-11-01-2.log"));
    }

    // 状态迁移：跨天序号归零，超限才递增
    void nextSlotResetsOnNewDayAndGrowsOnOverflow()
    {
        const QDate yesterday(2026, 10, 31);
        const QDate today(2026, 11, 1);
        // 同一天且没超限：继续写当前文件
        QCOMPARE(LogService::nextSlot(today, 1, 1000, today, 5000).rotate, 1);
        // 同一天超限：序号 +1
        QCOMPARE(LogService::nextSlot(today, 1, 6000, today, 5000).rotate, 2);
        // 跨天：无论之前转到几，序号都归零
        QCOMPARE(LogService::nextSlot(yesterday, 7, 6000, today, 5000).rotate, 0);
    }

    // 端到端：小阈值下真的翻出第二个文件，且当前文件跟着走
    void rotatesToNextFileWhenOverLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LogService::install(dir.filePath(QStringLiteral("logs")), 400);
        const QString padding = QStringLiteral("x").repeated(120);
        for (int i = 0; i < 8; ++i)
            qWarning() << padding;

        const QString logs = dir.filePath(QStringLiteral("logs"));
        const QStringList entries = QDir(logs).entryList(QDir::Files);
        QVERIFY2(entries.size() >= 2, qPrintable(entries.join(QLatin1Char(',')))); // 转出了 -1
        const QString current = LogService::currentLogFile();
        QVERIFY2(current.contains(QStringLiteral("-1.log")) || current.contains(QStringLiteral("-2.log")),
                 qPrintable(current)); // 当前写的就是轮转后的文件
    }

    // 只轮转不清理跑几个月能攒几十个文件；启动时扫一遍，过期直接删
    void prunesLogsOlderThanRetention()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString logs = dir.filePath(QStringLiteral("logs"));
        QVERIFY(QDir().mkpath(logs));

        const auto makeAged = [&logs](const QString& name, int daysOld) {
            QFile file(logs + QLatin1Char('/') + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
            // setFileTime 只能对开着的目标调（Qt 要求），写完先改时间再关
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-daysOld),
                                     QFileDevice::FileModificationTime));
            file.close();
        };
        makeAged(QStringLiteral("2026-10-01.log"), 8);   // 超期：该删
        makeAged(QStringLiteral("2026-10-30.log"), 3);   // 新鲜：该留
        makeAged(QStringLiteral("2026-10-30-1.log"), 3); // 轮转件同样看 mtime
        QFile note(logs + QStringLiteral("/keep.txt"));  // 非日志文件不得误删
        QVERIFY(note.open(QIODevice::WriteOnly));
        note.write("keep");
        note.close();

        LogService::install(logs);

        QVERIFY2(!QFileInfo::exists(logs + QStringLiteral("/2026-10-01.log")),
                 "超过保留期的日志没删掉");
        QVERIFY(QFileInfo::exists(logs + QStringLiteral("/2026-10-30.log")));
        QVERIFY(QFileInfo::exists(logs + QStringLiteral("/2026-10-30-1.log")));
        QVERIFY(QFileInfo::exists(logs + QStringLiteral("/keep.txt")));
    }

    // 文件固定 UTF-8，控制台按本地码页：直接喷 UTF-8 到中文 Windows 控制台就是乱码
    void fileStaysUtf8WhileConsoleUsesLocalEncoding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LogService::install(dir.filePath(QStringLiteral("logs")));
        qWarning() << QStringLiteral("桥接就绪");

        QFile file(LogService::currentLogFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY2(bytes.contains(QString::fromUtf8("桥接就绪").toUtf8()),
                 "日志文件必须是 UTF-8，否则换台机器又读不出来");
        QVERIFY2(LogService::consoleBytes(QStringLiteral("桥接就绪"))
                     == QStringLiteral("桥接就绪").toLocal8Bit(),
                 "控制台输出得用本地码页，不能直接喷 UTF-8 字节");
        LogService::shutdown();
    }

    // 只有 warning 及以上进事件桥：info/debug 仅落盘，避免日志洪水
    void forwardsWarningAndErrorOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QObject context;
        int received = 0;
        QJsonObject last;
        LogService::install(dir.filePath(QStringLiteral("logs")));
        LogService::setEventSink(
            &context, [&received, &last](const QJsonObject& entry) {
                ++received;
                last = entry;
            });

        qInfo() << QStringLiteral("不该转发");
        qWarning() << QStringLiteral("该转发");
        qCritical() << QStringLiteral("该转发");
        QTest::qWait(300); // 事件桥走 QueuedConnection，等回主线程

        QCOMPARE(received, 2);
        QCOMPARE(last.value(QStringLiteral("level")).toString(), QStringLiteral("error"));
        QVERIFY(last.value(QStringLiteral("message")).toString().contains(QStringLiteral("该转发")));
        QVERIFY(last.keys().contains(QStringLiteral("time")));
    }

    // sink 内部再打日志不得重新入队，否则一条警告能把自己刷爆
    void sinkLogsDoNotRecurse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QObject context;
        int received = 0;
        std::function<void(const QJsonObject&)> sink;
        sink = [&received](const QJsonObject&) {
            ++received;
            qWarning() << QStringLiteral("sink 里再打一条"); // 不得再次入队
        };
        LogService::install(dir.filePath(QStringLiteral("logs")));
        LogService::setEventSink(&context, sink);

        qWarning() << QStringLiteral("第一条");
        QTest::qWait(500);

        QCOMPARE(received, 1); // 只入队一次：sink 内的 qWarning 被 g_inSink 挡掉
    }
};

QTEST_MAIN(LogServiceTest)
#include "tst_log_service.moc"
