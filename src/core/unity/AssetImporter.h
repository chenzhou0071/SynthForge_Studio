#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

class AssetLibrary;
class McpRuntime;
class BlenderGate;

// 「添加资产」链路：校验附件 → 需要时经 Blender 把 .glb 转成 .fbx → 交 Unity 导入。
// 转换与 Blender 任务共用同一把 BlenderGate（owner=unity-convert），全程持有、finishPrepare 释放。
class AssetImporter : public QObject
{
    Q_OBJECT
public:
    using PrepareCallback = std::function<void(bool ok, const QStringList& fbxPaths,
                                               const QString& error)>;

    AssetImporter(AssetLibrary* library, McpRuntime* blender, McpRuntime* unity,
                  QObject* parent = nullptr);
    void setBlenderGate(BlenderGate* gate) { m_gate = gate; }

    bool hasPairedFbx(const QString& glbPath) const;
    QString fbxPathFor(const QString& glbPath) const;
    void prepare(const QJsonArray& attachments, PrepareCallback done);
    void importToUnity(const QString& fbxAbsolutePath, const QJsonObject& options,
                       std::function<void(bool, const QString&)> done);

private:
    void continuePrepare();
    void finishPrepare(bool ok, const QString& error);
    void ensureFbx(const QString& glbAbsolutePath,
                   std::function<void(bool, const QString&, const QString&)> done);

    AssetLibrary* m_library;
    McpRuntime* m_blender;
    McpRuntime* m_unity;
    BlenderGate* m_gate = nullptr;
    bool m_gateHeld = false;
    QJsonArray m_attachments;
    int m_prepareIndex = 0;
    QStringList m_fbxPaths;
    PrepareCallback m_prepareDone;
};
