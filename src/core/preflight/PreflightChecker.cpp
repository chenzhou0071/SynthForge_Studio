#include "core/preflight/PreflightChecker.h"

#include "core/mcp/McpRuntime.h"
#include "core/settings/SettingsStore.h"

#include <QJsonObject>

PreflightChecker::PreflightChecker(SettingsStore* settings, McpRuntime* blenderRuntime)
    : m_settings(settings)
    , m_blenderRuntime(blenderRuntime)
{
}

PreflightResult PreflightChecker::checkLowpoly() const
{
    if (m_blenderRuntime && m_blenderRuntime->isReady())
        return {true, QString()};
    return {false, QStringLiteral("blender-not-ready")};
}

PreflightResult PreflightChecker::checkCloud3d() const
{
    const QJsonObject cloud = m_settings
                                  ? m_settings->data().value(QStringLiteral("cloud3d")).toObject()
                                  : QJsonObject{};
    const QString provider =
        cloud.value(QStringLiteral("provider")).toString(QStringLiteral("tripo"));
    const QJsonObject config = cloud.value(provider).toObject();

    bool ready = false;
    if (provider == QLatin1String("tripo")) {
        ready = !config.value(QStringLiteral("apiKey")).toString().isEmpty();
    } else if (provider == QLatin1String("hunyuan")) {
        ready = !config.value(QStringLiteral("secretId")).toString().isEmpty()
                && !config.value(QStringLiteral("secretKey")).toString().isEmpty();
    } else if (provider == QLatin1String("custom")) {
        ready = !config.value(QStringLiteral("baseUrl")).toString().isEmpty();
    }

    return ready ? PreflightResult{true, QString()}
                 : PreflightResult{false, QStringLiteral("cloud3d-not-configured")};
}
