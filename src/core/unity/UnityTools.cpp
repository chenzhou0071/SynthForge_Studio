#include "core/unity/UnityTools.h"

#include <QJsonObject>
#include <QStringList>

QJsonArray filterUnityTools(const QJsonArray& all)
{
    static const QStringList prefixes{QStringLiteral("manage_"), QStringLiteral("create_"),
                                      QStringLiteral("read_"), QStringLiteral("find_"),
                                      QStringLiteral("import_"), QStringLiteral("refresh_"),
                                      QStringLiteral("validate_"), QStringLiteral("batch_")};
    QJsonArray kept;
    for (const QJsonValue& value : all) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        for (const QString& prefix : prefixes) {
            if (name.startsWith(prefix)) {
                kept.append(value); // 原样保留：description/inputSchema 要交给 LLM
                break;
            }
        }
    }
    return kept;
}
