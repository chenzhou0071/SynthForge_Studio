#pragma once

#include <QDate>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

// 全局日志接管：按天写文件、超限轮转，并把 warning 以上转发为诊断事件。
// 必须在 QCoreApplication 构造之后调用 install()/setEventSink()。
class LogService
{
public:
    static constexpr qint64 kDefaultMaxBytes = 50LL * 1024 * 1024; // 单文件 50MB 轮转
    static constexpr int kKeepDays = 7; // 启动时清掉更早的日志，防目录无限生长

    // 当前写入位置：日期 + 当天轮转序号
    struct Slot
    {
        QDate date;
        int rotate = 0;
    };

    static void install(const QString& logDir, qint64 maxBytes = kDefaultMaxBytes);
    static void shutdown();
    static QString currentLogFile();
    static void setEventSink(QObject* context, std::function<void(const QJsonObject&)> sink);

    // 纯函数：命名与轮转状态迁移，单独抽出来才能不依赖系统日期地测跨天归零
    static QString filePathFor(const QString& dir, const QDate& date, int rotate);
    static Slot nextSlot(const QDate& writtenDate, int writtenRotate, qint64 writtenSize,
                         const QDate& today, qint64 maxBytes);
};
