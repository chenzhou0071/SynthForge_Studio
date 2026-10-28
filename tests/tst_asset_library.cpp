#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
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

    // 云生成来源要落盘：重启应用后文件树仍能标出「云 · glb」
    void cloudAssetSourceIsPersisted()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Props")));
        QFile file(dir.path() + QStringLiteral("/Props/crate.glb"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        lib.markCloudAsset(QStringLiteral("Props/crate.glb"), QStringLiteral("tripo"));
        QCOMPARE(lib.cloudAssets().value(QStringLiteral("Props/crate.glb")),
                 QStringLiteral("tripo"));

        AssetLibrary reopened(dir.path());
        QCOMPARE(reopened.cloudAssets().value(QStringLiteral("Props/crate.glb")),
                 QStringLiteral("tripo"));
    }

    void cloudAssetSourceFollowsRenameAndDelete()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Props")));
        QFile file(dir.path() + QStringLiteral("/Props/crate.glb"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        lib.markCloudAsset(QStringLiteral("Props/crate.glb"), QStringLiteral("hunyuan"));
        QVERIFY(lib.renameEntry(QStringLiteral("Props/crate.glb"), QStringLiteral("box.glb"))
                    .isEmpty());
        QCOMPARE(lib.cloudAssets().value(QStringLiteral("Props/box.glb")),
                 QStringLiteral("hunyuan"));
        QVERIFY(!lib.cloudAssets().contains(QStringLiteral("Props/crate.glb")));

        QVERIFY(lib.deleteEntry(QStringLiteral("Props/box.glb")).isEmpty());
        QVERIFY(lib.cloudAssets().isEmpty());
    }

    void metadataFolderStaysHidden()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AssetLibrary lib(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/Props")));
        QFile file(dir.path() + QStringLiteral("/Props/crate.glb"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        lib.markCloudAsset(QStringLiteral("Props/crate.glb"), QStringLiteral("custom"));

        QVERIFY(!lib.scanFolders().contains(QStringLiteral(".synthforge")));
        for (const QJsonValue& child : lib.folderTree().value(QStringLiteral("children")).toArray())
            QVERIFY(child.toObject().value(QStringLiteral("name")).toString()
                    != QStringLiteral(".synthforge"));
    }
};

QTEST_MAIN(AssetLibraryTest)
#include "tst_asset_library.moc"
