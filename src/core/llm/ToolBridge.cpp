#include "core/llm/ToolBridge.h"

#include <QJsonDocument>
#include <QJsonObject>

QJsonArray ToolBridge::mcpToolsToOpenAi(const QJsonArray& mcpTools)
{
    QJsonArray out;
    for (const QJsonValue& value : mcpTools) {
        const QJsonObject tool = value.toObject();
        QJsonObject parameters = tool.value("inputSchema").toObject();
        if (parameters.isEmpty())
            parameters = QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}};
        out.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{{"name", tool.value("name")},
                                     {"description", tool.value("description")},
                                     {"parameters", parameters}}},
        });
    }
    return out;
}

QJsonArray ToolBridge::internalCallsToWire(const QJsonArray& calls)
{
    QJsonArray out;
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QJsonObject arguments = call.value("arguments").toObject();
        out.append(QJsonObject{
            {"id", call.value("id")},
            {"type", "function"},
            {"function", QJsonObject{
                {"name", call.value("name")},
                {"arguments", QString::fromUtf8(
                    QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}},
        });
    }
    return out;
}
