#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/assets/AssetLibrary.h"

// 资产库只负责根目录内的路径与文件操作；越界（../、绝对路径）一律拒绝。
class AssetLibraryTest : public QObject
{
    Q_OBJECT

private slots:
    void folderOpsStayInsideRoot()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());

        QVERIFY(lib.makeFolder(QString(), QStringLiteral("Props")).isEmpty());
        QVERIFY(QDir(dir.path() + QStringLiteral("/Props")).exists());
        QCOMPARE(lib.makeFolder(QString(), QStringLiteral("Props")),
                 QStringLiteral("already-exists"));
        QCOMPARE(lib.makeFolder(QStringLiteral(".."), QStringLiteral("Escape")),
                 QStringLiteral("invalid-path"));
        QCOMPARE(lib.makeFolder(QString(), QStringLiteral("../Escape")),
                 QStringLiteral("invalid-path"));

        QVERIFY(lib.renameEntry(QStringLiteral("Props"), QStringLiteral("Props2")).isEmpty());
        QVERIFY(QDir(dir.path() + QStringLiteral("/Props2")).exists());
        QVERIFY(lib.deleteEntry(QStringLiteral("Props2")).isEmpty());
        QVERIFY(!QDir(dir.path() + QStringLiteral("/Props2")).exists());
        QCOMPARE(lib.deleteEntry(QStringLiteral("Props2")), QStringLiteral("not-found"));
    }

    void validTargetRejectsEscape()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());

        QVERIFY(lib.isValidTarget(QString()));
        QVERIFY(lib.isValidTarget(QStringLiteral("Props")));
        QVERIFY(!lib.isValidTarget(QStringLiteral("../outside")));
        QVERIFY(!lib.isValidTarget(QStringLiteral("Props/../../outside")));
    }

    void scanAssetsFindsSiblings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Buildings")));
        for (const QString& name : {QStringLiteral("shop.glb"), QStringLiteral("shop.blend"),
                                    QStringLiteral("notes.txt")}) {
            QFile file(dir.path() + QStringLiteral("/Buildings/") + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
        }

        const QStringList assets = lib.scanAssets();
        QCOMPARE(assets.size(), 2); // txt 不计入
        QVERIFY(assets.contains(QStringLiteral("Buildings/shop.glb")));
        QVERIFY(assets.contains(QStringLiteral("Buildings/shop.blend")));
    }
    // 空目录也必须能被扫到：左栏树不能只靠模型文件反推目录
    void scanFoldersFindsEmptyNested()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Empty/Nested")));
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Props")));

        const QStringList folders = lib.scanFolders();
        QCOMPARE(folders.size(), 3);
        QVERIFY(folders.contains(QStringLiteral("Empty")));
        QVERIFY(folders.contains(QStringLiteral("Empty/Nested")));
        QVERIFY(folders.contains(QStringLiteral("Props")));
    }
};

QTEST_MAIN(AssetLibraryTest)
#include "tst_asset_library.moc"
