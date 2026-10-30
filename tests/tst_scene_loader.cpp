#include <QtTest>

#include "core/agent/SceneLoader.h"

class SceneLoaderTest : public QObject
{
    Q_OBJECT

private slots:
    void scriptOnlyOpensTargetNoBackup()
    {
        // 确认弹窗已经明说「会清空当前场景」，那就是让用户自己去保存；
        // 系统再自动 save 一份备份纯属捣乱，故脚本只打开、绝不再写任何文件
        const QString script = sceneLoadScript(QStringLiteral("d:/lib/Props/desk.blend"));
        QVERIFY(script.contains(QStringLiteral("bpy.ops.wm.open_mainfile")));
        QVERIFY(script.contains(QStringLiteral("desk.blend")));
        QVERIFY(!script.contains(QStringLiteral("save_as_mainfile")));
        QVERIFY(!script.contains(QStringLiteral("makedirs")));
        QVERIFY(!script.contains(QStringLiteral("backup")));
        QVERIFY(!script.contains(QStringLiteral("save_version")));
    }

    void singleQuoteInPathIsEscapedNotConcatenated()
    {
        // 资产基名允许用户自己填，含引号时不能拼出可执行表达式
        const QString script = sceneLoadScript(QStringLiteral("d:/it's.blend"));
        QVERIFY(script.contains(QStringLiteral("it\\'s.blend")));
    }
};

QTEST_GUILESS_MAIN(SceneLoaderTest)
#include "tst_scene_loader.moc"
