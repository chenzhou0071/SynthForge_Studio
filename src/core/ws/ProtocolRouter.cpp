#include "core/ws/ProtocolRouter.h"

#include <stdexcept>
#include <utility>

void ProtocolRouter::registerHandler(const QString& method, Handler handler)
{
    m_handlers.insert(method, std::move(handler));
}

QJsonValue ProtocolRouter::dispatch(const QString& method, const QJsonObject& params) const
{
    const auto it = m_handlers.constFind(method);
    if (it == m_handlers.constEnd())
        throw std::runtime_error("unknown method: " + method.toStdString());
    return it.value()(params);
}
