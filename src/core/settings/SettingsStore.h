#pragma once

#include <QJsonObject>
#include <QString>

QString defaultDataDir(); // %APPDATA%/SynthForgeStudio
QJsonObject makeDefaultSettings();

class SettingsStore
{
public:
    explicit SettingsStore(QString dataDir);

    // 文件不存在 → 写入默认值；已存在 → 与默认值做浅合并（保留未知键）
    void load();
    bool save();
    QJsonObject data() const { return m_data; }
    void setData(const QJsonObject& value) { m_data = value; }

private:
    QString m_path;
    QJsonObject m_data;
};
