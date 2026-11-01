#include "core/logging/LogService.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTime>

#include <cstdio>

namespace {

// 状态一律放函数内静态：Qt 容器（QString/QDate/std::function）如果在 CRT 静态初始化
// 期就构造，链接顺序不巧会踩到尚未初始化的 QtCore，直接崩在 main() 之前。
struct State
{
    QFile* file = nullptr;
    QString dir;
    QString path;      // 当前写入的文件；空 = 还没打开
    QDate date;        // 当前文件对应的日期；无效 = 尚未打开
    int rotate = 0;
    qint64 maxBytes = LogService::kDefaultMaxBytes;
    QMutex mutex;
    QPointer<QObject> sinkContext;
    std::function<void(const QJsonObject&)> sink;
};

State& state()
{
    static State s;
    return s;
}

// sink 里再打日志不得重新入队，否则一条警告能把自己刷爆
thread_local bool g_inSink = false;

// 必须先 close 再 delete：QFile 析构时对未关闭文件会打 qWarning，
// 而那条告警会再次进入正持着同一把非递归锁的 handler，直接死锁。
void closeAndDrop(QFile*& file)
{
    if (!file)
        return;
    file->close();
    delete file;
    file = nullptr;
}

// 只轮转不清理会让日志目录无限生长：启动时扫一遍，修改时间早于保留期的直接删
void purgeExpired(const QString& dir, int keepDays)
{
    if (keepDays <= 0)
        return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-keepDays);
    QDirIterator it(dir, {QStringLiteral("*.log")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).lastModified() < cutoff)
            QFile::remove(path);
    }
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    State& s = state();
    const QString category =
        QString::fromUtf8(context.category ? context.category : "app");
    const char level = type == QtDebugMsg     ? 'D'
                       : type == QtInfoMsg    ? 'I'
                       : type == QtWarningMsg ? 'W'
                                              : 'E';
    const QString line = QStringLiteral("[%1] [%2] %3: %4\n")
                             .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                             .arg(QLatin1Char(level))
                             .arg(category, message);
    {
        QMutexLocker lock(&s.mutex);
        const LogService::Slot slot = LogService::nextSlot(
            s.date, s.rotate, s.file ? s.file->size() : 0, QDate::currentDate(), s.maxBytes);
        const QString wanted = LogService::filePathFor(s.dir, slot.date, slot.rotate);
        if (!s.file || s.path != wanted) { // 跨天 / 轮转 / 首次：换文件重开
            closeAndDrop(s.file);
            QDir().mkpath(s.dir);
            s.file = new QFile(wanted);
            if (s.file->open(QIODevice::Append | QIODevice::WriteOnly)) {
                s.path = wanted;
                s.date = slot.date;
                s.rotate = slot.rotate;
            } else {
                closeAndDrop(s.file); // 打不开就只走 stderr，不记错误路径以免下次不重试
                s.path = QString();
            }
        }
        if (s.file) {
            s.file->write(line.toUtf8());
            s.file->flush(); // 崩溃前的日志也要留在盘上
        }
    }
    std::fprintf(stderr, "%s", LogService::consoleBytes(line).constData());
    std::fflush(stderr);

    // 诊断事件桥：只有 warning 及以上转发，info/debug 仅落盘
    if (!s.sink || !s.sinkContext || g_inSink)
        return;
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg)
        return;
    const QJsonObject entry{
        {QStringLiteral("level"),
         type == QtWarningMsg ? QStringLiteral("warning") : QStringLiteral("error")},
        {QStringLiteral("category"), category},
        {QStringLiteral("message"), message},
        {QStringLiteral("time"), QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))}};
    QMetaObject::invokeMethod(
        s.sinkContext,
        [entry]() {
            if (!state().sink)
                return;
            g_inSink = true;
            state().sink(entry);
            g_inSink = false;
        },
        Qt::QueuedConnection); // 日志可能来自任意线程，广播只在 WS 线程做
}
} // namespace

QString LogService::filePathFor(const QString& dir, const QDate& date, int rotate)
{
    const QString suffix =
        rotate > 0 ? QStringLiteral("-%1").arg(rotate) : QString();
    return dir + QLatin1Char('/') + date.toString(QStringLiteral("yyyy-MM-dd")) + suffix
           + QStringLiteral(".log");
}

QByteArray LogService::consoleBytes(const QString& line)
{
    // 控制台按系统本地码页解释字节（中文 Windows 是 GBK）；直接喷 UTF-8 就是乱码。
    // 子进程透传过来的也是本地码页原文，两者保持一致才能同屏共存。
    return line.toLocal8Bit();
}

LogService::Slot LogService::nextSlot(const QDate& writtenDate, int writtenRotate,
                                      qint64 writtenSize, const QDate& today, qint64 maxBytes)
{
    if (writtenDate != today)
        return {today, 0}; // 跨天：当天序号归零
    if (writtenSize > maxBytes)
        return {today, writtenRotate + 1};
    return {today, writtenRotate};
}

void LogService::install(const QString& logDir, qint64 maxBytes)
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    s.dir = logDir;
    s.maxBytes = maxBytes;
    closeAndDrop(s.file);
    s.path = QString();
    s.date = QDate();
    s.rotate = 0;
    QDir().mkpath(s.dir);
    purgeExpired(s.dir, kKeepDays);
    qInstallMessageHandler(handler);
}

QString LogService::currentLogFile()
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    return s.path;
}

void LogService::setEventSink(QObject* context, std::function<void(const QJsonObject&)> sink)
{
    state().sinkContext = context;
    state().sink = std::move(sink);
}

void LogService::shutdown()
{
    qInstallMessageHandler(nullptr); // 先摘 handler：Qt 会就此打一条 Debug，走旧路径无妨
    State& s = state();
    QMutexLocker lock(&s.mutex);
    s.sink = nullptr;
    s.sinkContext = nullptr;
    closeAndDrop(s.file);
    s.path = QString();
    s.date = QDate();
    s.rotate = 0;
}
