#include "core/cloud3d/Cloud3dProvider.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

QString Cloud3dProvider::failureText(QNetworkReply* reply, const QByteArray& body)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    QString message = obj.value(QStringLiteral("message")).toString();
    if (message.isEmpty())
        message = obj.value(QStringLiteral("error")).toString();
    if (message.isEmpty())
        message = reply->errorString();

    // 4xx 是配置/参数问题，重试没有意义；网络层失败与 5xx 交给 Service 重试
    const bool retryable = status < 400 || status >= 500;
    return (retryable ? QStringLiteral("retryable:") : QString()) + message;
}
