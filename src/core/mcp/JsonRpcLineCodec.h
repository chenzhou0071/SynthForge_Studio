#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

struct JsonRpcLine
{
    enum class Kind { Response, ErrorResponse, Notification };

    Kind kind = Kind::Response;
    qint64 id = 0;          // Response / ErrorResponse 有效
    QJsonValue result;      // Response
    QJsonObject error;      // ErrorResponse：{code, message, data?}
    QString method;         // Notification
    QJsonValue params;      // Notification（对象或数组，保持原始 JSON 形态）
};

class JsonRpcLineCodec
{
public:
    // 解析一行（不含换行）。失败返回 false 并写入 error。
    // P1 约定：含 method 的一律视为通知（stdio 场景下服务端极少主动发请求）。
    static bool parse(const QByteArray& line, JsonRpcLine* out, QString* error);

    // 序列化请求 / 通知，返回值已含尾部 '\n'。
    static QByteArray makeRequest(qint64 id, const QString& method, const QJsonObject& params);
    static QByteArray makeNotification(const QString& method, const QJsonObject& params);
};
