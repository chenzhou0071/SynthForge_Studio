#include "core/unity/AssetImporter.h"

#include "core/assets/AssetLibrary.h"
#include "core/mcp/BlenderGate.h"
#include "core/mcp/McpClient.h"
#include "core/mcp/McpRuntime.h"

#include <QDir>
#include <QFileInfo>

#include <utility>

AssetImporter::AssetImporter(AssetLibrary* library, McpRuntime* blender, McpRuntime* unity,
                             QObject* parent)
    : QObject(parent), m_library(library), m_blender(blender), m_unity(unity)
{
}

bool AssetImporter::hasPairedFbx(const QString& glbPath) const
{
    return QFileInfo::exists(fbxPathFor(glbPath));
}

QString AssetImporter::fbxPathFor(const QString& glbPath) const
{
    const QFileInfo info(glbPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".fbx"));
}

void AssetImporter::prepare(const QJsonArray& attachments, PrepareCallback done)
{
    m_attachments = attachments;
    m_prepareIndex = 0;
    m_fbxPaths.clear();
    m_prepareDone = std::move(done);
    continuePrepare();
}

void AssetImporter::continuePrepare()
{
    while (m_prepareIndex < m_attachments.size()) {
        const QString relative = m_attachments.at(m_prepareIndex).toString();
        const QString absolute = m_library ? m_library->absoluteFor(relative) : QString();
        if (relative.isEmpty() || !m_library || !m_library->isValidTarget(relative)
            || !QFileInfo::exists(absolute)) {
            finishPrepare(false, QStringLiteral("invalid-attachment"));
            return;
        }
        if (absolute.endsWith(QLatin1String(".fbx"), Qt::CaseInsensitive)) {
            m_fbxPaths << absolute;
            ++m_prepareIndex;
            continue;
        }
        if (!absolute.endsWith(QLatin1String(".glb"), Qt::CaseInsensitive)) {
            finishPrepare(false, QStringLiteral("invalid-attachment"));
            return;
        }
        if (hasPairedFbx(absolute)) {
            m_fbxPaths << fbxPathFor(absolute);
            ++m_prepareIndex;
            continue;
        }
        if (m_gate && !m_gateHeld) {
            // 先门禁后链就绪：忙时直接返回 blender-busy，不启动转换；成功后持有到整组附件处理完
            if (!m_gate->tryAcquire(QStringLiteral("unity-convert"))) {
                finishPrepare(false, QStringLiteral("blender-busy"));
                return;
            }
            m_gateHeld = true;
        }
        ensureFbx(absolute, [this](bool ok, const QString& fbx, const QString& error) {
            if (!ok) {
                finishPrepare(false, error);
                return;
            }
            m_fbxPaths << fbx;
            ++m_prepareIndex;
            continuePrepare();                  // 异步转换完一个，继续下一个
        });
        return;                                 // 等待转换回调
    }
    finishPrepare(true, QString());
}

void AssetImporter::finishPrepare(bool ok, const QString& error)
{
    if (m_gateHeld && m_gate)
        m_gate->release(QStringLiteral("unity-convert"));
    m_gateHeld = false;
    PrepareCallback done = m_prepareDone;
    const QStringList paths = m_fbxPaths;
    m_prepareDone = nullptr;
    m_attachments = {};
    m_prepareIndex = 0;
    if (done)
        done(ok, paths, error);
}

void AssetImporter::ensureFbx(
    const QString& glb, std::function<void(bool, const QString&, const QString&)> done)
{
    if (!m_blender || !m_blender->isReady()) {
        done(false, {}, QStringLiteral("blender-not-ready"));
        return;
    }
    const QString fbx = fbxPathFor(glb);
    const QString script =
        QStringLiteral("import bpy\n"
                       "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
                       "bpy.ops.import_scene.gltf(filepath=r'%1')\n"
                       "bpy.ops.export_scene.fbx(filepath=r'%2', use_selection=False, "
                       "apply_scale_options='FBX_SCALE_ALL', path_mode='COPY')\n")
            .arg(glb, fbx);
    m_blender->client()->callTool(
        QStringLiteral("execute_blender_code"), QJsonObject{{"code", script}},
        [done = std::move(done), fbx](const QJsonValue&, const QJsonObject& error) {
            if (!error.isEmpty() || !QFileInfo::exists(fbx)) {
                done(false, {}, QStringLiteral("blender-convert-failed"));
                return;
            }
            done(true, fbx, QString());
        });
}

void AssetImporter::importToUnity(const QString& fbx, const QJsonObject& options,
                                  std::function<void(bool, const QString&)> done)
{
    if (!m_unity || !m_unity->isReady()) {
        done(false, QStringLiteral("unity-not-ready"));
        return;
    }
    QJsonObject args{{"path", fbx}};
    if (!options.value("instanceName").toString().isEmpty())
        args.insert(QStringLiteral("name"), options.value("instanceName"));
    if (options.contains(QStringLiteral("position")))
        args.insert(QStringLiteral("position"), options.value("position"));
    m_unity->client()->callTool(
        QStringLiteral("import_model_file"), args,
        [done = std::move(done)](const QJsonValue&, const QJsonObject& error) {
            if (!error.isEmpty()) {
                done(false, QStringLiteral("import-failed"));
                return;
            }
            done(true, QString());
        });
}
