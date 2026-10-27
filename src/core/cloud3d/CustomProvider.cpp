#include "core/cloud3d/CustomProvider.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace {
QString mapState(const QString& raw)
{
    if (raw == QLatin1String("queued"))
        return QStringLiteral("queued");
    if (raw == QLatin1String("success"))
        return QStringLiteral("success");
    if (raw == QLatin1String("failed") || raw == QLatin1String("cancelled"))
        return QStringLiteral("failed");
    return QStringLiteral("running"); // running 及未知状态都继续轮询
}

QString trimmedBase(QString baseUrl)
{
    while (baseUrl.endsWith(QLatin1Char('/')))
        baseUrl.chop(1);
    return baseUrl;
}
} // namespace

CustomProvider::CustomProvider(QString baseUrl, QString apiToken, QObject* parent)
    : Cloud3dProvider(parent)
    , m_baseUrl(trimmedBase(std::move(baseUrl)))
    , m_apiToken(std::move(apiToken))
{
}

void CustomProvider::submit(const QString& prompt, const QString& model)
{
    QJsonObject body;
    body.insert(QStringLiteral("prompt"), prompt);
    if (!model.isEmpty())
        body.insert(QStringLiteral("model"), model);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/task")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiToken.toUtf8());

    QNetworkReply* reply =
        m_nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString reason = failureText(reply, data);
            reply->deleteLater();
            emit failed(reason);
            return;
        }
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        const QString taskId = obj.value(QStringLiteral("task_id")).toString();
        if (!taskId.isEmpty()) {
            emit submitted(taskId);
            return;
        }

        QString message = obj.value(QStringLiteral("message")).toString();
        if (message.isEmpty())
            message = obj.value(QStringLiteral("error")).toString();
        if (message.isEmpty())
            message = QStringLiteral("自定义服务提交失败：响应里没有 task_id");
        emit failed(message);
    });
}

void CustomProvider::poll(const QString& taskId)
{
    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/task/") + taskId));
    if (!m_apiToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiToken.toUtf8());

    QNetworkReply* reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString reason = failureText(reply, data);
            reply->deleteLater();
            emit failed(reason);
            return;
        }
        reply->deleteLater();

        const QJsonObject payload = QJsonDocument::fromJson(data).object();
        if (payload.isEmpty()) {
            emit failed(QStringLiteral("自定义服务查询失败：响应不是 JSON 对象"));
            return;
        }

        Cloud3dStatus result;
        result.state = mapState(payload.value(QStringLiteral("status")).toString());
        result.percent = qBound(0, payload.value(QStringLiteral("progress")).toInt(), 100);
        result.modelUrl = payload.value(QStringLiteral("model_url")).toString();
        if (result.state == QLatin1String("failed")) {
            result.error = payload.value(QStringLiteral("error")).toString();
            if (result.error.isEmpty())
                result.error = payload.value(QStringLiteral("message")).toString();
            if (result.error.isEmpty())
                result.error = QStringLiteral("生成失败");
        }
        if (result.state == QLatin1String("success") && result.modelUrl.isEmpty()) {
            emit failed(QStringLiteral("自定义服务返回成功但缺少 model_url"));
            return;
        }
        emit status(result);
    });
}
