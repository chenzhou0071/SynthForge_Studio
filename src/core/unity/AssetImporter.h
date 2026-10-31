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
    // glb→fbx 的 Blender 脚本：定向清空场景（绝不用 read_factory_settings，那会重置偏好、
    // 关掉 BlenderMCP 插件导致断连），导出时带贴图避免白模。抽成静态便于单测。
    static QString glbToFBXScript(const QString& glb, const QString& fbx);
    // 在 graceMs 内每 250ms 查一次文件：MCP 回执可能因 Blender 排队/桥抖动而超时，
    // 但命令往往已经执行、产物迟到落地。文件出现即算成功。异步，不阻塞事件循环。
    static void waitForFile(const QString& path, int graceMs, std::function<void(bool)> done);
    void prepare(const QJsonArray& attachments, PrepareCallback done);
    void importToUnity(const QString& fbxAbsolutePath, const QJsonObject& options,
                       std::function<void(bool, const QString&)> done);

private:
    static void waitForFileStep(const QString& path, int graceMs, int waited,
                                std::function<void(bool)> done);
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
