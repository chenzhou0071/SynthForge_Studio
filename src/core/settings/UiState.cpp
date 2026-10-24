#include "core/settings/UiState.h"

#include <QDir>
#include <QSettings>

namespace {
const QString kGeometryKey = QStringLiteral("window/geometry");
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
