#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// 资产库：只管理根目录内的路径与文件，越界（../、绝对路径）一律拒绝。
// 根目录默认 Documents/SynthForgeStudio/ModelLibrary，由 CoreApp 创建。
class AssetLibrary
{
public:
    explicit AssetLibrary(QString root);

    QString root() const { return m_root; }

    QJsonObject folderTree() const;                      // 递归目录树（仅目录），供目录弹窗
    bool isValidTarget(const QString& relative) const;   // canonical 前缀校验，防 ../ 逃逸
    QString absoluteFor(const QString& relative) const;
    QString relativeFor(const QString& absolute) const;  // 越界或不存在返回空
    QStringList scanAssets() const;                      // glb + fbx + blend，统一正斜杠相对路径
    QStringList scanFolders() const;                     // 全部子目录（含空目录），供左栏树展示

    // 云生成来源：落盘在 <root>/.synthforge/cloud-assets.json，重启后仍能标「云 · glb」
    QHash<QString, QString> cloudAssets() const; // 相对路径 -> provider id
    void markCloudAsset(const QString& relative, const QString& provider);

    // 文件操作：返回错误码，空串 = 成功
    QString makeFolder(const QString& parentRelative, const QString& name);
    QString renameEntry(const QString& relative, const QString& newName);
    QString deleteEntry(const QString& relative);
    bool revealEntry(const QString& relative) const;

private:
    bool isInsideRoot(const QString& absolute) const;
    void writeCloudAssets(const QHash<QString, QString>& assets);
    QString m_root;
};
