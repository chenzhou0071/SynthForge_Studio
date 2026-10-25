#include "core/assets/AssetLibrary.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>

#include <functional>

namespace {

bool hasIllegalName(const QString& name)
{
    return name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
           || name.contains('/') || name.contains('\\');
}

} // namespace

AssetLibrary::AssetLibrary(QString root)
    : m_root(QDir::cleanPath(std::move(root)))
{
}

QJsonObject AssetLibrary::folderTree() const
{
    std::function<QJsonArray(const QString&, int)> walk =
        [this, &walk](const QString& absolute, int depth) -> QJsonArray {
        QJsonArray children;
        if (depth > 6) // 深度上限：避免资产库过深时把整棵树塞进弹窗
            return children;
        const QFileInfoList entries =
            QDir(absolute).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& entry : entries) {
            children.append(QJsonObject{{QStringLiteral("name"), entry.fileName()},
                                        {QStringLiteral("path"),
                                         relativeFor(entry.absoluteFilePath())},
                                        {QStringLiteral("children"),
                                         walk(entry.absoluteFilePath(), depth + 1)}});
        }
        return children;
    };
    return QJsonObject{{QStringLiteral("name"), QFileInfo(m_root).fileName()},
                       {QStringLiteral("path"), QString()},
                       {QStringLiteral("children"), walk(m_root, 1)}};
}

bool AssetLibrary::isValidTarget(const QString& relative) const
{
    const QString root = QFileInfo(m_root).canonicalFilePath();
    if (root.isEmpty())
        return false;
    // 目标目录可能还不存在，只用 cleanPath 消掉 ../ 再比前缀，不依赖 canonical
    const QString candidate = QDir::cleanPath(QDir(root).absoluteFilePath(relative));
    return candidate == root || candidate.startsWith(root + '/');
}

QString AssetLibrary::absoluteFor(const QString& relative) const
{
    if (relative.isEmpty())
        return m_root;
    return QDir::cleanPath(QDir(m_root).absoluteFilePath(relative));
}

QString AssetLibrary::relativeFor(const QString& absolute) const
{
    const QString root = QFileInfo(m_root).canonicalFilePath();
    const QString path = QFileInfo(absolute).canonicalFilePath();
    if (path.isEmpty() || (path != root && !path.startsWith(root + '/')))
        return {};
    return QDir(root).relativeFilePath(path);
}

QStringList AssetLibrary::scanAssets() const
{
    QStringList result;
    QDirIterator it(m_root, {QStringLiteral("*.glb"), QStringLiteral("*.fbx"),
                             QStringLiteral("*.blend")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        result << QDir(m_root).relativeFilePath(it.next()); // 统一正斜杠相对路径
    return result;
}

QStringList AssetLibrary::scanFolders() const
{
    QStringList result;
    QDirIterator it(m_root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        result << QDir(m_root).relativeFilePath(it.next());
    return result;
}

QString AssetLibrary::makeFolder(const QString& parentRelative, const QString& name)
{
    if (hasIllegalName(name))
        return QStringLiteral("invalid-path");
    const QString parent = absoluteFor(parentRelative);
    if (!isInsideRoot(parent) || !QDir(parent).exists())
        return QStringLiteral("invalid-path");
    const QString target = QDir(parent).filePath(name);
    if (QFileInfo::exists(target))
        return QStringLiteral("already-exists");
    return QDir().mkpath(target) ? QString() : QStringLiteral("io-error");
}

QString AssetLibrary::renameEntry(const QString& relative, const QString& newName)
{
    if (hasIllegalName(newName))
        return QStringLiteral("invalid-path");
    const QString absolute = absoluteFor(relative);
    if (!isInsideRoot(absolute) || !QFileInfo::exists(absolute))
        return QStringLiteral("not-found");
    const QString target = QFileInfo(absolute).dir().filePath(newName);
    if (QFileInfo::exists(target))
        return QStringLiteral("already-exists");
    return QDir().rename(absolute, target) ? QString() : QStringLiteral("io-error");
}

QString AssetLibrary::deleteEntry(const QString& relative)
{
    const QString absolute = absoluteFor(relative);
    if (relative.isEmpty() || !isInsideRoot(absolute) || !QFileInfo::exists(absolute))
        return QStringLiteral("not-found");
    const QFileInfo info(absolute);
    const bool ok = info.isDir() ? QDir(absolute).removeRecursively() : QFile::remove(absolute);
    return ok ? QString() : QStringLiteral("io-error");
}

bool AssetLibrary::revealEntry(const QString& relative) const
{
    const QString absolute = absoluteFor(relative);
    if (!isInsideRoot(absolute) || !QFileInfo::exists(absolute))
        return false;
    return QProcess::startDetached(QStringLiteral("explorer.exe"),
                                   {QStringLiteral("/select,"),
                                    QDir::toNativeSeparators(absolute)});
}

bool AssetLibrary::isInsideRoot(const QString& absolute) const
{
    const QString root = QFileInfo(m_root).canonicalFilePath();
    const QString path = QFileInfo(absolute).canonicalFilePath();
    return !path.isEmpty() && (path == root || path.startsWith(root + '/'));
}
