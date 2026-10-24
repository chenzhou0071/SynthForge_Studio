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
};

QTEST_GUILESS_MAIN(UiStateTest)
#include "tst_ui_state.moc"
