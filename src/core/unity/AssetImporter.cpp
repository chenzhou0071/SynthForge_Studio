#include "core/unity/AssetImporter.h"

#include "core/assets/AssetLibrary.h"
#include "core/mcp/BlenderGate.h"
#include "core/mcp/McpClient.h"
#include "core/mcp/McpRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <utility>

namespace {
// 转换请求自身的超时：Blender 单线程、上一条命令没跑完时新请求只能排队，
// 照 30s 默认超时会在排队阶段就被误判死（命令其实随后照常执行）。
constexpr int kConvertTimeoutMs = 120000;
// 回执正常但文件还没出现：导出是同步写盘，只给落盘/杀毒扫描留一点余量
constexpr int kQuietGraceMs = 3000;
// 回执失败（超时/桥抖动）：命令可能还在 Blender 队列里，宽限窗口要长一些
constexpr int kLateGraceMs = 20000;
constexpr int kPollIntervalMs = 250;
} // namespace

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

QString AssetImporter::glbToFBXScript(const QString& glb, const QString& fbx)
{
    // 定向清空（不用 read_factory_settings：它会重置偏好、关掉 BlenderMCP 插件→断连）；
    // 导出带 embed_textures，避免进 Unity 变白模。
    return QStringLiteral("import bpy\n"
                          "if bpy.context.object and bpy.context.object.mode != 'OBJECT':\n"
                          "    bpy.ops.object.mode_set(mode='OBJECT')\n"
                          "bpy.ops.object.select_all(action='SELECT')\n"
                          "bpy.ops.object.delete(use_global=False)\n"
                          "for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images,\n"
                          "             bpy.data.armatures, bpy.data.actions, bpy.data.cameras,"
                          " bpy.data.lights):\n"
                          "    for item in list(coll):\n"
                          "        if item.users == 0:\n"
                          "            coll.remove(item)\n"
                          "bpy.ops.import_scene.gltf(filepath=r'%1')\n"
                          "bpy.ops.export_scene.fbx(filepath=r'%2', use_selection=False,\n"
                          "                        apply_scale_options='FBX_SCALE_ALL',\n"
                          "                        path_mode='COPY', embed_textures=True)\n")
        .arg(glb, fbx);
}

void AssetImporter::waitForFile(const QString& path, int graceMs,
                                std::function<void(bool)> done)
{
    waitForFileStep(path, graceMs, 0, std::move(done));
}

void AssetImporter::waitForFileStep(const QString& path, int graceMs, int waited,
                                    std::function<void(bool)> done)
{
    if (QFileInfo::exists(path)) {
        done(true);
        return;
    }
    if (waited >= graceMs) {
        done(false);
        return;
    }
    QTimer::singleShot(kPollIntervalMs, QCoreApplication::instance(),
                       [path, graceMs, waited, done = std::move(done)]() mutable {
                           AssetImporter::waitForFileStep(path, graceMs,
                                                          waited + kPollIntervalMs,
                                                          std::move(done));
                       });
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
    const QString script = glbToFBXScript(glb, fbx);
    m_blender->client()->callTool(
        QStringLiteral("execute_blender_code"), QJsonObject{{"code", script}},
        [done = std::move(done), fbx](const QJsonValue&, const QJsonObject& error) {
            // 回执不可信：超时只删本地 pending，Blender 端脚本往往仍在排队/执行，
            // fbx 会迟到落地；以文件为唯一事实，宽限窗口内出现即算成功。
            if (QFileInfo::exists(fbx)) {
                done(true, fbx, QString());
                return;
            }
            const int graceMs = error.isEmpty() ? kQuietGraceMs : kLateGraceMs;
            waitForFile(fbx, graceMs, [done = std::move(done), fbx](bool appeared) {
                if (appeared)
                    done(true, fbx, QString());
                else
                    done(false, {}, QStringLiteral("blender-convert-timeout"));
            });
        },
        QJsonValue(), kConvertTimeoutMs); // 转换要等 Blender 空闲，不能照 30s 默认超时
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
