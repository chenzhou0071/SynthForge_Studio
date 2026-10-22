#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

class ProtocolRouter
{
public:
    using Handler = std::function<QJsonValue(const QJsonObject& params)>;

    void registerHandler(const QString& method, Handler handler);
    // 返回 result；未注册的方法抛 std::runtime_error，由 WsServer 转成错误帧
    QJsonValue dispatch(const QString& method, const QJsonObject& params) const;

private:
    QHash<QString, Handler> m_handlers;
};
