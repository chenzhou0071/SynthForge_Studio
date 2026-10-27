#include "core/cloud3d/Tc3Signature.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QStringList>
#include <QTimeZone>

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QString tc3Authorization(const QString& secretId, const QString& secretKey,
                         const QString& service, const QString& host,
                         const QByteArray& payload, qint64 unixSeconds,
                         const QString& action, const QByteArray& contentType)
{
    // 腾讯云按 UTC 日期做密钥派生；本地时区偏移会让 Credential 日期和签名对不上
    const QDateTime utc = QDateTime::fromSecsSinceEpoch(unixSeconds, QTimeZone::UTC);
    const QString date = utc.date().toString(QStringLiteral("yyyy-MM-dd"));

    const QByteArray hashedPayload =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    // 参与签名的头部按 key 的 ASCII 升序排列；content-type 与 host 必选，
    // x-tc-action 可选（签了才能防止接口名在传输中被替换）。
    QString canonicalHeaders = QStringLiteral("content-type:")
        + QString::fromLatin1(contentType) + QStringLiteral("\nhost:") + host
        + QLatin1Char('\n');
    QString signedHeaders = QStringLiteral("content-type;host");
    if (!action.isEmpty()) {
        canonicalHeaders +=
            QStringLiteral("x-tc-action:") + action.toLower() + QLatin1Char('\n');
        signedHeaders += QStringLiteral(";x-tc-action");
    }

    const QString canonicalRequest =
        QStringList{QStringLiteral("POST"), QStringLiteral("/"), QString(), canonicalHeaders,
                    signedHeaders, QString::fromLatin1(hashedPayload)}
            .join(QLatin1Char('\n'));

    const QString stringToSign =
        QStringList{
            QStringLiteral("TC3-HMAC-SHA256"), QString::number(unixSeconds),
            date + QLatin1Char('/') + service + QStringLiteral("/tc3_request"),
            QString::fromLatin1(
                QCryptographicHash::hash(canonicalRequest.toUtf8(), QCryptographicHash::Sha256)
                    .toHex())}
            .join(QLatin1Char('\n'));

    const QByteArray secretDate = hmacSha256("TC3" + secretKey.toUtf8(), date.toUtf8());
    const QByteArray secretService = hmacSha256(secretDate, service.toUtf8());
    const QByteArray secretSigning = hmacSha256(secretService, "tc3_request");
    const QString signature =
        QString::fromLatin1(hmacSha256(secretSigning, stringToSign.toUtf8()).toHex());

    return QStringLiteral("TC3-HMAC-SHA256 Credential=%1/%2/%3/tc3_request, "
                          "SignedHeaders=%4, Signature=%5")
        .arg(secretId, date, service, signedHeaders, signature);
}
