#include "core/cloud3d/TripoProvider.h"

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
    if (raw == QLatin1String("failed") || raw == QLatin1String("cancelled")
        || raw == QLatin1String("banned") || raw == QLatin1String("expired"))
        return QStringLiteral("failed");
    return QStringLiteral("running"); // running 及未知状态都继续轮询
}

QString trimmedBase(QString baseUrl)
{
    while (baseUrl.endsWith(QLatin1Char('/')))
        baseUrl.chop(1);
    return baseUrl;
}

void setBearer(QNetworkRequest& request, const QString& apiKey)
{
    if (!apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
}
} // namespace

TripoProvider::TripoProvider(QString apiKey, QString version, QString baseUrl, QObject* parent)
    : Cloud3dProvider(parent)
    , m_apiKey(std::move(apiKey))
    , m_version(std::move(version))
    , m_baseUrl(trimmedBase(std::move(baseUrl)))
{
}

void TripoProvider::submit(const QString& prompt, const QString& model)
{
    QJsonObject body;
    body.insert(QStringLiteral("prompt"), prompt);
    // V3 的 model 可选，留空即用服务端默认版本（v3.1-20260211）
    const QString version = (model.isEmpty() ? m_version : model).trimmed();
    if (!version.isEmpty())
        body.insert(QStringLiteral("model"), version);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v3/generation/text-to-model")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    setBearer(request, m_apiKey);

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
        const QString taskId = obj.value(QStringLiteral("data"))
                                   .toObject()
                                   .value(QStringLiteral("task_id"))
                                   .toString();
        if (!taskId.isEmpty()) {
            emit submitted(taskId);
            return;
        }

        QString message = obj.value(QStringLiteral("message")).toString();
        if (message.isEmpty())
            message = QStringLiteral("Tripo 提交失败：响应里没有 task_id");
        emit failed(message);
    });
}

void TripoProvider::poll(const QString& taskId)
{
    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v3/tasks/") + taskId));
    setBearer(request, m_apiKey);

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

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        const QJsonObject payload = obj.value(QStringLiteral("data")).toObject();
        if (payload.isEmpty()) {
            QString message = obj.value(QStringLiteral("message")).toString();
            if (message.isEmpty())
                message = QStringLiteral("Tripo 查询失败：响应里没有 data");
            emit failed(message);
            return;
        }

        Cloud3dStatus result;
        result.state = mapState(payload.value(QStringLiteral("status")).toString());
        result.percent = qBound(0, payload.value(QStringLiteral("progress")).toInt(), 100);
        result.modelUrl = payload.value(QStringLiteral("output"))
                              .toObject()
                              .value(QStringLiteral("model_url"))
                              .toString();
        if (result.state == QLatin1String("failed")) {
            result.error = payload.value(QStringLiteral("error_message")).toString();
            if (result.error.isEmpty())
                result.error = payload.value(QStringLiteral("error")).toString();
            if (result.error.isEmpty())
                result.error = payload.value(QStringLiteral("message")).toString();
            if (result.error.isEmpty())
                result.error = QStringLiteral("Tripo 生成失败");
        }
        if (result.state == QLatin1String("success") && result.modelUrl.isEmpty()) {
            emit failed(QStringLiteral("Tripo 返回成功但缺少模型地址"));
            return;
        }
        emit status(result);
    });
}
