#include "core/settings/UiState.h"

#include <QDir>
#include <QSettings>

#include "core/agent/ShapePolicy.h"

namespace {
const QString kGeometryKey = QStringLiteral("window/geometry");
const QString kBlenderModeKey = QStringLiteral("blender/mode");
const QString kChatModeKey = QStringLiteral("chat/mode");
const QString kShapeKey = QStringLiteral("output/shape");
}

UiState::UiState(QString dataDir)
    : m_path(QDir(dataDir).filePath(QStringLiteral("ui.ini")))
{
}

QByteArray UiState::windowGeometry() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    return settings.value(kGeometryKey).toByteArray();
}

void UiState::setWindowGeometry(const QByteArray& geometry)
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue(kGeometryKey, geometry);
}

QString UiState::blenderMode() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    const QString mode = settings.value(kBlenderModeKey).toString();
    // 文件被手改坏时回默认，别把界面顶成两个按钮都不选中的状态
    return mode == QLatin1String("cloud3d") ? mode : QStringLiteral("lowpoly");
}

void UiState::setBlenderMode(const QString& mode)
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue(kBlenderModeKey, mode);
}

QString UiState::chatMode() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    const QString mode = settings.value(kChatModeKey).toString();
    return mode == QLatin1String("ask") ? mode : QStringLiteral("exec");
}

void UiState::setChatMode(const QString& mode)
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue(kChatModeKey, mode);
}

QString UiState::shape() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    // 合法键表只在 ShapePolicy 一份：过一遍回落，免得 settings/handler/策略表三处各写一份
    return shapeKey(shapeFromString(settings.value(kShapeKey).toString()));
}

void UiState::setShape(const QString& shape)
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue(kShapeKey, shape);
}
