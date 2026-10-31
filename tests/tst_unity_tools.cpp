#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest>
#include "core/unity/UnityTools.h"

class UnityToolsTest : public QObject
{
    Q_OBJECT
private slots:
    // 只放行编辑器操作类前缀；generate_* / execute_code / delete_* 不在白名单
    void keepsWhitelistedPrefixes()
    {
        const QStringList names{
            "manage_gameobject", "create_script", "read_console", "find_gameobjects",
            "import_model_file", "refresh_unity", "validate_script", "batch_execute",
            "generate_image",    "generate_model", "execute_code", "delete_script"};
        QJsonArray all;
        for (const QString& name : names)
            all.append(QJsonObject{{QStringLiteral("name"), name}});

        QSet<QString> kept;
        for (const QJsonValue& value : filterUnityTools(all))
            kept.insert(value.toObject().value(QStringLiteral("name")).toString());

        for (const QString& name : {"manage_gameobject", "create_script", "read_console",
                                    "find_gameobjects", "import_model_file", "refresh_unity",
                                    "validate_script", "batch_execute"})
            QVERIFY2(kept.contains(name), qPrintable(name + " 应被放行"));
        for (const QString& name : {"generate_image", "generate_model", "execute_code",
                                    "delete_script"})
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
