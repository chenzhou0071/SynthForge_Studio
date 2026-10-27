#pragma once

#include "core/cloud3d/Cloud3dProvider.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>

#include <functional>

// 腾讯混元生 3D：异步任务，提交拿 JobId，再轮询结果；每个请求都要 TC3 签名。
class HunyuanProvider : public Cloud3dProvider
{
    Q_OBJECT
public:
    // baseUrl 可指向本地 mock；生产默认混元生 3D 域名
    explicit HunyuanProvider(QString secretId, QString secretKey, QString region, QString model,
                             QString baseUrl =
                                 QStringLiteral("https://ai3d.tencentcloudapi.com"),
                             QObject* parent = nullptr);

    void submit(const QString& prompt, const QString& model) override;
    void poll(const QString& taskId) override;

private:
    // 统一处理签名、TC3 公共头和 Response.Error，回调里只处理业务字段
    void send(const QString& action, const QJsonObject& body,
              std::function<void(const QJsonObject&)> onResponse);

    QNetworkAccessManager m_nam;
    QString m_secretId;
    QString m_secretKey;
    QString m_region;
    QString m_model; // 预留给设置页；当前接口没有 Model 字段，不发送
    QString m_baseUrl;
};
