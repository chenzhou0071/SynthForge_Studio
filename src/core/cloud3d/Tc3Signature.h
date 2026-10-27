#pragma once

#include <QByteArray>
#include <QString>

// 腾讯云 API 3.0 的 TC3-HMAC-SHA256 签名；混元 3D 的每个请求都靠它鉴权。
QByteArray hmacSha256(const QByteArray& key, const QByteArray& message);

// 返回 Authorization 头值。unixSeconds 用 UTC 秒级时间戳，签名日期也由它推导。
// action 非空时把 x-tc-action 一并纳入签名（官方示例做法）；
// contentType 必须与实际请求头逐字符一致，否则服务端复算会对不上。
QString tc3Authorization(const QString& secretId, const QString& secretKey,
                         const QString& service, const QString& host,
                         const QByteArray& payload, qint64 unixSeconds,
                         const QString& action = QString(),
                         const QByteArray& contentType =
                             QByteArrayLiteral("application/json; charset=utf-8"));
