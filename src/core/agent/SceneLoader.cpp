#include "core/agent/SceneLoader.h"

#include "core/assets/AssetLibrary.h"

#include <QDir>
#include <QFileInfo>

namespace {

// Python 单引号字符串字面量：反斜杠与单引号都要转义，
// 这样资产基名里即便含引号也只会被当作字符串内容，拼不出可执行表达式。
QString pyLiteral(const QString& text)
{
    QString escaped = text;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"))
        .replace(QStringLiteral("'"), QStringLiteral("\\'"));
    return QStringLiteral("'%1'").arg(escaped);
}

} // namespace

QString resolveBlendSource(const AssetLibrary& library, const QString& assetPath)
{
    if (assetPath.isEmpty())
        return {};
    const QFileInfo picked(library.absoluteFor(assetPath));
    // 选中项本身是 .blend 就用它；否则取同基名的 .blend（云资产只有 glb，返回空）
    const QString blend = picked.suffix().compare(QLatin1String("blend"), Qt::CaseInsensitive) == 0
                              ? picked.absoluteFilePath()
                              : QDir(picked.dir().absolutePath())
                                    .filePath(picked.completeBaseName() + QStringLiteral(".blend"));
    return QFileInfo::exists(blend) ? blend : QString();
}

QString sceneLoadScript(const QString& targetBlend)
{
    return QStringLiteral(
               "import bpy\n"
               "target = %1\n"
               "bpy.ops.wm.open_mainfile(filepath=target)\n"
               "print('LOADED=' + target)\n")
        .arg(pyLiteral(targetBlend));
}
