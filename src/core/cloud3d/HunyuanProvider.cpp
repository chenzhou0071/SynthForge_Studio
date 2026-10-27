#include "core/cloud3d/HunyuanProvider.h"

#include "core/cloud3d/Tc3Signature.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace {
constexpr auto kService = "ai3d";
constexpr auto kVersion = "2025-05-13";
constexpr auto kContentType = "application/json; charset=utf-8";

// 腾讯云的错误可能藏在 HTTP 200 的 Response.Error 里；网络错误则沿用统一分类
QString responseError(const QJsonObject& response)
{
    return response.value(QStringLiteral("Error"))
        .toObject()
        .value(QStringLiteral("Message"))
        .toString();
}
} // namespace

HunyuanProvider::HunyuanProvider(QString secretId, QString secretKey, QString region,
                                 QString model, QString baseUrl, QObject* parent)
    : Cloud3dProvider(parent)
    , m_secretId(std::move(secretId))
    , m_secretKey(std::move(secretKey))
    , m_region(std::move(region))
    , m_model(std::move(model))
    , m_baseUrl(std::move(baseUrl))
{
}

void HunyuanProvider::submit(const QString& prompt, const QString& model)
{
    Q_UNUSED(model) // 当前接口没有模型档位字段，m_model 仅预留给设置页

    QJsonObject body;
    body.insert(QStringLiteral("Prompt"), prompt);
    // 不指定格式时混元默认只给 OBJ，下游要的是 GLB
    body.insert(QStringLiteral("ResultFormat"), QStringLiteral("GLB"));

    send(QStringLiteral("SubmitHunyuanTo3DJob"), body, [this](const QJsonObject& response) {
        const QString jobId = response.value(QStringLiteral("JobId")).toString();
        if (jobId.isEmpty()) {
            emit failed(QStringLiteral("混元提交失败：响应里没有 JobId"));
            return;
        }
        emit submitted(jobId);
    });
}

void HunyuanProvider::poll(const QString& taskId)
{
    QJsonObject body;
    body.insert(QStringLiteral("JobId"), taskId);

    send(QStringLiteral("QueryHunyuanTo3DJob"), body, [this](const QJsonObject& response) {
        const QString raw = response.value(QStringLiteral("Status")).toString();

        Cloud3dStatus result;
        if (raw == QLatin1String("WAIT")) {
            result.state = QStringLiteral("queued");
        } else if (raw == QLatin1String("DONE")) {
            result.state = QStringLiteral("success");
            result.percent = 100;
            const QJsonArray files = response.value(QStringLiteral("ResultFile3Ds")).toArray();
            for (const QJsonValue& value : files) {
                const QJsonObject file = value.toObject();
                if (file.value(QStringLiteral("Type")).toString().compare(
                        QLatin1String("GLB"), Qt::CaseInsensitive)
                    == 0) {
                    result.modelUrl = file.value(QStringLiteral("Url")).toString();
                    break;
                }
            }
            if (result.modelUrl.isEmpty()) {
                emit failed(QStringLiteral("混元返回成功但缺少 GLB 文件"));
                return;
            }
        } else if (raw == QLatin1String("FAIL")) {
            result.state = QStringLiteral("failed");
            result.error = response.value(QStringLiteral("ErrorMessage")).toString();
            if (result.error.isEmpty())
                result.error = QStringLiteral("生成失败");
        } else {
            result.state = QStringLiteral("running");
            result.percent = 50;
        }
        emit status(result);
    });
}

void HunyuanProvider::send(const QString& action, const QJsonObject& body,
                           std::function<void(const QJsonObject&)> onResponse)
{
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QUrl url(m_baseUrl);
    const QString host = url.authority();
    const qint64 timestamp = QDateTime::currentSecsSinceEpoch();

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(kContentType));
    request.setRawHeader("X-TC-Action", action.toUtf8());
    request.setRawHeader("X-TC-Version", kVersion);
    request.setRawHeader("X-TC-Timestamp", QByteArray::number(timestamp));
    if (!m_region.isEmpty())
        request.setRawHeader("X-TC-Region", m_region.toUtf8());
    // 只签 content-type 与 host：两者一定存在，也不会因可选头缺失而对不上
    const QString authorization = tc3Authorization(m_secretId, m_secretKey,
                                                   QString::fromLatin1(kService), host, payload,
                                                   timestamp);
    request.setRawHeader("Authorization", authorization.toUtf8());

    QNetworkReply* reply = m_nam.post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, onResponse = std::move(onResponse)]() {
        const QByteArray data = reply->readAll();
        const QJsonObject response =
            QJsonDocument::fromJson(data).object().value(QStringLiteral("Response")).toObject();
        const bool httpError = reply->error() != QNetworkReply::NoError;
        QString reason = httpError ? failureText(reply, data) : QString();
        reply->deleteLater();

        if (httpError) {
            const QString message = responseError(response);
            if (!message.isEmpty()) {
                const bool retryable = reason.startsWith(QStringLiteral("retryable:"));
                reason = (retryable ? QStringLiteral("retryable:") : QString()) + message;
            }
            emit failed(reason);
            return;
        }

        // HTTP 200 也可能是业务错误，例如密钥不合法
        const QString message = responseError(response);
        if (!message.isEmpty()) {
            emit failed(message);
            return;
        }
        onResponse(response);
    });
}
