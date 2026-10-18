#include "core/mcp/JsonRpcLineCodec.h"

#include <QJsonDocument>

bool JsonRpcLineCodec::parse(const QByteArray& line, JsonRpcLine* out, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("JSON parse error: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        if (error)
            *error = QStringLiteral("unsupported jsonrpc version");
        return false;
    }

    if (obj.value(QStringLiteral("method")).isString()) {
        out->kind = JsonRpcLine::Kind::Notification;
        out->method = obj.value(QStringLiteral("method")).toString();
        out->params = obj.value(QStringLiteral("params"));
        return true;
    }

    if (!obj.contains(QStringLiteral("id"))) {
        if (error)
            *error = QStringLiteral("line has neither method nor id");
        return false;
    }

    out->id = obj.value(QStringLiteral("id")).toVariant().toLongLong();
    if (obj.value(QStringLiteral("error")).isObject()) {
        out->kind = JsonRpcLine::Kind::ErrorResponse;
        out->error = obj.value(QStringLiteral("error")).toObject();
    } else {
        out->kind = JsonRpcLine::Kind::Response;
        out->result = obj.value(QStringLiteral("result"));
    }
    return true;
}

QByteArray JsonRpcLineCodec::makeRequest(qint64 id, const QString& method,
                                         const QJsonObject& params)
{
    QJsonObject obj{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty())
        obj.insert(QStringLiteral("params"), params);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray JsonRpcLineCodec::makeNotification(const QString& method,
                                              const QJsonObject& params)
{
    QJsonObject obj{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty())
        obj.insert(QStringLiteral("params"), params);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}
