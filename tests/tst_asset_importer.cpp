#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include "core/assets/AssetLibrary.h"
#include "core/mcp/BlenderGate.h"
#include "core/unity/AssetImporter.h"

class AssetImporterTest : public QObject
{
    Q_OBJECT
private slots:
    void fbxPairDetection()
    {
        QTemporaryDir dir;
        AssetImporter importer(nullptr, nullptr, nullptr);
        const QString glb = dir.path() + "/crate.glb";
        QVERIFY(!importer.hasPairedFbx(glb));
        QFile fbx(dir.path() + "/crate.fbx");
        QVERIFY(fbx.open(QIODevice::WriteOnly));
        QVERIFY(importer.hasPairedFbx(glb));
        QCOMPARE(importer.fbxPathFor(glb), dir.path() + "/crate.fbx");
    }

    void missingAttachmentIsRejected()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        AssetImporter importer(&library, nullptr, nullptr);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("nope.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("invalid-attachment"));
    }

    void glbWithoutFbxRequiresBlender()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        AssetImporter importer(&library, nullptr, nullptr);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-not-ready"));
    }

    void glbWithoutFbxWhileGateBusy()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        BlenderGate gate;
        QVERIFY(gate.tryAcquire(QStringLiteral("blender-agent")));
        AssetImporter importer(&library, nullptr, nullptr);
        importer.setBlenderGate(&gate);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-busy"));
        QCOMPARE(gate.owner(), QStringLiteral("blender-agent")); // 不释放他人租约
    }

    void failedConversionReleasesGate()
    {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QFile glb(dir.path() + "/crate.glb");
        QVERIFY(glb.open(QIODevice::WriteOnly));
        glb.write("glTF");
        glb.close();
        BlenderGate gate;
        AssetImporter importer(&library, nullptr, nullptr);
        importer.setBlenderGate(&gate);
        bool finished = false;
        QString error;
        importer.prepare(QJsonArray{QStringLiteral("crate.glb")},
                         [&](bool ok, const QStringList&, const QString& err) {
                             QVERIFY(!ok);
                             error = err;
                             finished = true;
                         });
        QTRY_VERIFY(finished);
        QCOMPARE(error, QStringLiteral("blender-not-ready"));
        QVERIFY(!gate.isBusy());               // 获取后因链未就绪失败，必须释放
    }
};

QTEST_MAIN(AssetImporterTest)
#include "tst_asset_importer.moc"
