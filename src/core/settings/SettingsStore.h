#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

QString defaultDataDir(); // %APPDATA%/SynthForgeStudio
// --data-dir <path> / --data-dir=<path> 覆盖默认目录；缺省或值为空时回落默认值。
// 自动测试用它指向临时目录，避免写到真实用户配置。
QString dataDirFromArgs(const QStringList& args);
QJsonObject makeDefaultSettings();
// 预置服务商只存 model + key，baseUrl 从 providerBaseUrls 表取；自定义服务商才带 baseUrl
QString resolveAgentBaseUrl(const QJsonObject& agent, const QJsonObject& settings);

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
