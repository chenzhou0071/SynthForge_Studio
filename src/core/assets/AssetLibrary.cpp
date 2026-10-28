#include "core/assets/AssetLibrary.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>

#include <functional>

namespace {

bool hasIllegalName(const QString& name)
{
    return name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
           || name.contains('/') || name.contains('\\');
}

// 云生成来源记在模型库内的隐藏目录，跟库一起搬家
QString cloudMetaPath(const QString& root)
{
    return root + QStringLiteral("/.synthforge/cloud-assets.json");
}

bool isMetaFolder(const QString& relative)
{
    return relative == QLatin1String(".synthforge")
           || relative.startsWith(QLatin1String(".synthforge/"));
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
            if (entry.fileName() == QLatin1String(".synthforge"))
                continue; // 元数据目录不进目录选择弹窗
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
    while (it.hasNext()) {
        const QString relative = QDir(m_root).relativeFilePath(it.next());
        if (isMetaFolder(relative))
            continue;
        result << relative; // 统一正斜杠相对路径
    }
    return result;
}

QStringList AssetLibrary::scanFolders() const
{
    QStringList result;
    QDirIterator it(m_root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString relative = QDir(m_root).relativeFilePath(it.next());
        if (isMetaFolder(relative))
            continue;
        result << relative;
    }
    return result;
}

QHash<QString, QString> AssetLibrary::cloudAssets() const
{
    QHash<QString, QString> result;
    QFile file(cloudMetaPath(m_root));
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QJsonObject assets = QJsonDocument::fromJson(file.readAll())
                                   .object()
                                   .value(QStringLiteral("assets"))
                                   .toObject();
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        const QString provider =
            it.value().toObject().value(QStringLiteral("provider")).toString();
        if (!provider.isEmpty())
            result.insert(it.key(), provider);
    }
    return result;
}

void AssetLibrary::markCloudAsset(const QString& relative, const QString& provider)
{
    if (relative.isEmpty() || provider.isEmpty() || !isValidTarget(relative))
        return;

    QHash<QString, QString> assets = cloudAssets();
    // 顺手清掉文件已不存在的记录，避免外部改名/删除留下孤儿
    for (auto it = assets.begin(); it != assets.end();) {
        if (!QFileInfo::exists(absoluteFor(it.key())))
            it = assets.erase(it);
        else
            ++it;
    }
    assets.insert(relative, provider);
    writeCloudAssets(assets);
}

void AssetLibrary::writeCloudAssets(const QHash<QString, QString>& assets)
{
    QJsonObject map;
    for (auto it = assets.cbegin(); it != assets.cend(); ++it)
        map.insert(it.key(), QJsonObject{{QStringLiteral("provider"), it.value()}});
    const QJsonObject doc{{QStringLiteral("version"), 1},
                          {QStringLiteral("assets"), map}};

    QDir().mkpath(m_root + QStringLiteral("/.synthforge"));
    QSaveFile file(cloudMetaPath(m_root));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
    file.commit();
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
    if (!QDir().rename(absolute, target))
        return QStringLiteral("io-error");

    // 云生成来源跟着改名走（文件或目录都按前缀迁移）
    const QHash<QString, QString> assets = cloudAssets();
    if (!assets.isEmpty()) {
        const QString newRelative = QDir(m_root).relativeFilePath(target);
        QHash<QString, QString> updated;
        bool changed = false;
        for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
            if (it.key() == relative) {
                updated.insert(newRelative, it.value());
                changed = true;
            } else if (it.key().startsWith(relative + QLatin1Char('/'))) {
                updated.insert(newRelative + it.key().mid(relative.size()), it.value());
                changed = true;
            } else {
                updated.insert(it.key(), it.value());
            }
        }
        if (changed)
            writeCloudAssets(updated);
    }
    return {};
}

QString AssetLibrary::deleteEntry(const QString& relative)
{
    const QString absolute = absoluteFor(relative);
    if (relative.isEmpty() || !isInsideRoot(absolute) || !QFileInfo::exists(absolute))
        return QStringLiteral("not-found");
    const QFileInfo info(absolute);
    const bool ok = info.isDir() ? QDir(absolute).removeRecursively() : QFile::remove(absolute);
    if (!ok)
        return QStringLiteral("io-error");

    QHash<QString, QString> assets = cloudAssets();
    bool changed = false;
    for (auto it = assets.begin(); it != assets.end();) {
        if (it.key() == relative || it.key().startsWith(relative + QLatin1Char('/'))) {
            it = assets.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        writeCloudAssets(assets);
    return {};
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
