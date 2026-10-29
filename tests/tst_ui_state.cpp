#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/settings/UiState.h"

class UiStateTest : public QObject
{
    Q_OBJECT

private slots:
    void missingFileYieldsEmptyGeometry()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(UiState(dir.path()).windowGeometry().isEmpty());
    }

    void geometrySurvivesNewInstance()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray blob = QByteArray::fromHex("0102030405");
        UiState(dir.path()).setWindowGeometry(blob);
        QCOMPARE(UiState(dir.path()).windowGeometry(), blob);
    }

    void stateFileSitsInDataDir()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UiState(dir.path()).setWindowGeometry(QByteArrayLiteral("x"));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("ui.ini"))));
    }

    void overwriteKeepsLatestValue()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UiState state(dir.path());
        state.setWindowGeometry(QByteArrayLiteral("first"));
        state.setWindowGeometry(QByteArrayLiteral("second"));
        QCOMPARE(UiState(dir.path()).windowGeometry(), QByteArrayLiteral("second"));
    }

    void separateDataDirsAreIndependent()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(a.isValid() && b.isValid());
        UiState(a.path()).setWindowGeometry(QByteArrayLiteral("a-geometry"));
        UiState(b.path()).setWindowGeometry(QByteArrayLiteral("b-geometry"));
        QCOMPARE(UiState(a.path()).windowGeometry(), QByteArrayLiteral("a-geometry"));
        QCOMPARE(UiState(b.path()).windowGeometry(), QByteArrayLiteral("b-geometry"));
    }

    // 低模/云API、讨论/执行：localStorage 在 app:// 下不落盘，改由后端 ui.ini 记住
    void modesHaveDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const UiState state(dir.path());
        QCOMPARE(state.blenderMode(), QStringLiteral("lowpoly"));
        QCOMPARE(state.chatMode(), QStringLiteral("exec"));
    }

    void modesSurviveNewInstance()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            UiState state(dir.path());
            state.setBlenderMode(QStringLiteral("cloud3d"));
            state.setChatMode(QStringLiteral("ask"));
        } // 实例析构后重开，模拟重启
        const UiState restored(dir.path());
        QCOMPARE(restored.blenderMode(), QStringLiteral("cloud3d"));
        QCOMPARE(restored.chatMode(), QStringLiteral("ask"));
    }

    void unknownStoredModeFallsBackToDefault()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UiState(dir.path()).setBlenderMode(QStringLiteral("cloud9"));
        UiState(dir.path()).setChatMode(QStringLiteral("chat"));
        const UiState restored(dir.path());
        QCOMPARE(restored.blenderMode(), QStringLiteral("lowpoly"));
        QCOMPARE(restored.chatMode(), QStringLiteral("exec"));
    }

    // 三档产出形态（道具/盒式舞台/开放场景）跟两个开关一样存 ui.ini
    void shapeDefaultsToDioramaAndSurvives()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QCOMPARE(UiState(dir.path()).shape(), QStringLiteral("diorama"));
        UiState(dir.path()).setShape(QStringLiteral("prop"));
        QCOMPARE(UiState(dir.path()).shape(), QStringLiteral("prop"));
        UiState(dir.path()).setShape(QStringLiteral("open"));
        QCOMPARE(UiState(dir.path()).shape(), QStringLiteral("open"));
    }

    void unknownShapeFallsBackToDiorama()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UiState(dir.path()).setShape(QStringLiteral("cinema"));
        QCOMPARE(UiState(dir.path()).shape(), QStringLiteral("diorama"));
    }
};

QTEST_GUILESS_MAIN(UiStateTest)
#include "tst_ui_state.moc"
