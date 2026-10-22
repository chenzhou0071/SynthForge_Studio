#include "core/settings/SettingsStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

QString defaultDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QJsonObject makeDefaultSettings()
{
    auto provider = [](const QString& id) {
        return QJsonObject{{QStringLiteral("provider"), id},
                           {QStringLiteral("baseUrl"), QString()},
                           {QStringLiteral("apiKey"), QString()},
                           {QStringLiteral("model"), QString()}};
    };
    return QJsonObject{
        {QStringLiteral("providerBaseUrls"),
         QJsonObject{{QStringLiteral("openai"), QStringLiteral("https://api.openai.com/v1")},
                     {QStringLiteral("doubao"),
                      QStringLiteral("https://ark.cn-beijing.volces.com/api/v3")},
                     {QStringLiteral("qwen"),
                      QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1")},
                     {QStringLiteral("zhipu"),
                      QStringLiteral("https://open.bigmodel.cn/api/paas/v4")},
                     {QStringLiteral("deepseek"),
                      QStringLiteral("https://api.deepseek.com/v1")}}},
        {QStringLiteral("agent"),
         QJsonObject{{QStringLiteral("primary"), provider(QStringLiteral("openai"))},
                     {QStringLiteral("summarizer"), provider(QStringLiteral("openai"))}}},
        {QStringLiteral("cloud3d"),
         QJsonObject{{QStringLiteral("provider"), QStringLiteral("tripo")},
                     {QStringLiteral("tripo"), QJsonObject{}},
                     {QStringLiteral("hunyuan"), QJsonObject{}},
                     {QStringLiteral("custom"), QJsonObject{}}}},
        {QStringLiteral("ui"), QJsonObject{}}};
}

SettingsStore::SettingsStore(QString dataDir)
    : m_data(makeDefaultSettings())
{
    QDir().mkpath(dataDir);
    m_path = dataDir + QStringLiteral("/settings.json");
}

void SettingsStore::load()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        save(); // 首次运行落盘默认值
        return;
    }
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    // 浅合并：默认值兜底 + 用户已有键覆盖（未知键原样保留）
    QJsonObject merged = m_data;
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    m_data = merged;
}

bool SettingsStore::save()
{
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    return true;
}
