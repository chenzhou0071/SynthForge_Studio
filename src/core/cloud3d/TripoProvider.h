#pragma once

#include "core/cloud3d/Cloud3dProvider.h"

#include <QNetworkAccessManager>
#include <QString>

class TripoProvider : public Cloud3dProvider
{
    Q_OBJECT
public:
    // baseUrl 可指向本地 mock；生产默认 Tripo OpenAPI 域名（.com 为中文站，国际站是 .ai）
    explicit TripoProvider(QString apiKey, QString version,
                           QString baseUrl = QStringLiteral("https://openapi.tripo3d.com"),
                           QObject* parent = nullptr);

    void submit(const QString& prompt, const QString& model) override;
    void poll(const QString& taskId) override;

private:
    QNetworkAccessManager m_nam;
    QString m_apiKey;
    QString m_version;
    QString m_baseUrl;
};
