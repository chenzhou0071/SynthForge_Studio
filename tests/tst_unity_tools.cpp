#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest>
#include "core/unity/UnityTools.h"

class UnityToolsTest : public QObject
{
    Q_OBJECT
private slots:
    // 白名单：除“实机游玩”外的干活工具都放行（改脚本/删脚本/跑测试/execute_code）；
    // 另一套 AI 生成、多实例切换、底层调试类仍挡在外面。
    void keepsWhitelistedPrefixes()
    {
        const QStringList names{
            "manage_gameobject", "create_script", "read_console", "find_gameobjects",
            "import_model_file", "refresh_unity", "validate_script", "batch_execute",
            "apply_text_edits",  "script_apply_edits", "delete_script", "run_tests",
            "execute_code",      "get_test_job",       "generate_image", "generate_model",
            "get_sha",           "execute_menu_item",  "set_active_instance"};
        QJsonArray all;
        for (const QString& name : names)
            all.append(QJsonObject{{QStringLiteral("name"), name}});

        QSet<QString> kept;
        for (const QJsonValue& value : filterUnityTools(all))
            kept.insert(value.toObject().value(QStringLiteral("name")).toString());

        for (const QString& name : {"manage_gameobject", "create_script", "read_console",
                                    "find_gameobjects", "import_model_file", "refresh_unity",
                                    "validate_script", "batch_execute", "apply_text_edits",
                                    "script_apply_edits", "delete_script", "run_tests",
                                    "execute_code", "get_test_job"})
            QVERIFY2(kept.contains(name), qPrintable(name + " 应被放行"));
        for (const QString& name : {"generate_image", "generate_model", "get_sha",
                                    "execute_menu_item", "set_active_instance"})
            QVERIFY2(!kept.contains(name), qPrintable(name + " 应被挡掉"));
    }

    // 过滤只按 name 决定去留，原样保留工具对象其余字段（schema 要交给 LLM）
    void preservesOriginalToolObjects()
    {
        QJsonArray all;
        all.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("manage_scene")},
                               {QStringLiteral("description"), QStringLiteral("d")},
                               {QStringLiteral("inputSchema"), QJsonObject{{"type", "object"}}}});
        const QJsonArray kept = filterUnityTools(all);
        QCOMPARE(kept.size(), 1);
        const QJsonObject tool = kept.at(0).toObject();
        QCOMPARE(tool.value(QStringLiteral("description")).toString(), QStringLiteral("d"));
        QVERIFY(tool.contains(QStringLiteral("inputSchema")));
    }
};

QTEST_GUILESS_MAIN(UnityToolsTest)
#include "tst_unity_tools.moc"
