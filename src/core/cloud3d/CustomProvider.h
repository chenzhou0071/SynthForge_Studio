#pragma once

#include "core/cloud3d/Cloud3dProvider.h"

#include <QNetworkAccessManager>
#include <QString>

class CustomProvider : public Cloud3dProvider
{
    Q_OBJECT
public:
    explicit CustomProvider(QString baseUrl, QString apiToken, QObject* parent = nullptr);

    void submit(const QString& prompt, const QString& model) override;
    void poll(const QString& taskId) override;

private:
    QNetworkAccessManager m_nam;
    QString m_baseUrl;
    QString m_apiToken;
};
