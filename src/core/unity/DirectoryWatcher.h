#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

// Unity 工程目录监控：递归注册 <project>/Assets，400ms 防抖合并、>200 条降级全量、
// MCP 工具调用期间用抑制窗口吞掉回声事件（双源去重）。切换工程时 stop() 丢旧事件。
class DirectoryWatcher : public QObject
{
    Q_OBJECT
public:
    explicit DirectoryWatcher(QObject* parent = nullptr);

    void watch(const QString& root);   // 递归注册；root 固定为 <project>/Assets
    void stop();                       // 移除全部监控 + 清空 pending + 重置抑制
    void beginSuppress();              // MCP 工具调用前：深度 +1，5s 上限
    void endSuppress();                // 调用后：深度归零再给 500ms 落盘宽限
    bool isIgnored(const QString& path) const;

    // 报告某个目录发生变化：生产里由 QFileSystemWatcher::directoryChanged 调用，
    // 测试里直接注入以确定性验证防抖/限流/抑制（不依赖 OS 通知，避免沙箱下不稳定）。
    void noteChange(const QString& absolutePath);

signals:
    // reason ∈ incremental|full；changedDirs 为相对 root 的目录，"." 表示根
    void treeChanged(const QString& reason, const QStringList& changedDirs);

private:
    void registerRecursive(const QString& dir);
    bool isSuppressed() const;
    void flush();

    QFileSystemWatcher m_fs;
    QTimer m_debounce;
    QSet<QString> m_pending;
    QString m_root;
    int m_suppressDepth = 0;
    qint64 m_suppressUntilMs = 0;
};
