#include "core/unity/UnityTools.h"

#include <QJsonObject>
#include <QSet>
#include <QStringList>

QJsonArray filterUnityTools(const QJsonArray& all)
{
    // 前缀白名单：除“实机游玩”外的编辑器/脚本/测试类操作都放行
    static const QStringList prefixes{QStringLiteral("manage_"), QStringLiteral("create_"),
                                      QStringLiteral("read_"), QStringLiteral("find_"),
                                      QStringLiteral("import_"), QStringLiteral("refresh_"),
                                      QStringLiteral("validate_"), QStringLiteral("batch_"),
                                      QStringLiteral("apply_"), QStringLiteral("script_"),
                                      QStringLiteral("delete_"), QStringLiteral("run_")};
    // 按名精确放行：execute_ / get_ 前缀太宽，只开这两个，避免误放 execute_menu_item / get_sha
    static const QSet<QString> extras{QStringLiteral("execute_code"),
                                      QStringLiteral("get_test_job")};
    QJsonArray kept;
    for (const QJsonValue& value : all) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        bool allowed = extras.contains(name);
        for (const QString& prefix : prefixes) {
            if (name.startsWith(prefix)) {
                allowed = true;
                break;
            }
        }
        if (allowed)
            kept.append(value); // 原样保留：description/inputSchema 要交给 LLM
    }
    return kept;
}
