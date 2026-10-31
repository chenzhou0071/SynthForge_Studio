#include "core/unity/DirectoryWatcher.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

DirectoryWatcher::DirectoryWatcher(QObject* parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &DirectoryWatcher::flush);
    connect(&m_fs, &QFileSystemWatcher::directoryChanged, this,
            &DirectoryWatcher::noteChange);
}

bool DirectoryWatcher::isIgnored(const QString& path) const
{
    // Windows 回调可能是反斜杠：先归一再按路径段匹配，忽略 Unity 派生目录与 .meta 边车文件。
    // 调用方应传入相对 root 的路径，避免系统临时目录等祖先路径里的同名段误伤。
    const QString normalized = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (normalized.endsWith(QLatin1String(".meta"), Qt::CaseInsensitive))
        return true;
    static const QSet<QString> names{QStringLiteral("Library"), QStringLiteral("Temp"),
                                     QStringLiteral("obj"), QStringLiteral("Logs")};
    const QStringList segments = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& segment : segments)
        if (names.contains(segment))
            return true;
    return false;
}

void DirectoryWatcher::registerRecursive(const QString& dir)
{
    if (!QFileInfo::exists(dir))
        return;
    if (isIgnored(QDir(m_root).relativeFilePath(dir)))
        return;
    if (!m_fs.directories().contains(dir))
        m_fs.addPath(dir);
    const QFileInfoList subs = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& sub : subs)
        registerRecursive(sub.absoluteFilePath());
}

void DirectoryWatcher::watch(const QString& root)
{
    stop();
    m_root = QFileInfo(root).absoluteFilePath();
    if (!QFileInfo::exists(m_root))
        return;
    registerRecursive(m_root);
}

void DirectoryWatcher::stop()
{
    m_debounce.stop();
    m_pending.clear();
    const QStringList dirs = m_fs.directories();
    if (!dirs.isEmpty())
        m_fs.removePaths(dirs);
    m_suppressDepth = 0;
    m_suppressUntilMs = 0;
    m_root.clear();
}

void DirectoryWatcher::beginSuppress()
{
    ++m_suppressDepth;
    // 长工具调用期间给足上限，避免中途掉出抑制窗口
    m_suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 5000;
}

void DirectoryWatcher::endSuppress()
{
    if (m_suppressDepth > 0)
        --m_suppressDepth;
    if (m_suppressDepth == 0)
        m_suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 500; // MCP 落盘宽限
}

bool DirectoryWatcher::isSuppressed() const
{
    return m_suppressDepth > 0
        || QDateTime::currentMSecsSinceEpoch() < m_suppressUntilMs;
}

void DirectoryWatcher::noteChange(const QString& path)
{
    // 用相对 root 的路径判断忽略：祖先目录（如系统临时目录）里的同名段不应误伤
    if (isIgnored(m_root.isEmpty() ? path : QDir(m_root).relativeFilePath(path)))
        return;
    if (!m_fs.directories().contains(path))
        registerRecursive(path);                    // 新目录：始终补注册
    if (isSuppressed()) {
        m_pending.clear();                          // 抑制窗口：丢弃（MCP 定向更新负责）
        return;
    }
    m_pending.insert(path);
    m_debounce.start(400);                          // 单次定时器自动重置
}

void DirectoryWatcher::flush()
{
    if (m_pending.isEmpty())
        return;
    if (isSuppressed()) {
        m_pending.clear();
        return;
    }
    QStringList dirs;
    for (const QString& path : m_pending)
        dirs << QDir(m_root).relativeFilePath(path);   // "." 表示根
    m_pending.clear();
    if (dirs.size() > 200) {
        emit treeChanged(QStringLiteral("full"), {});   // 限流：降级全量刷新
        return;
    }
    emit treeChanged(QStringLiteral("incremental"), dirs);
}
