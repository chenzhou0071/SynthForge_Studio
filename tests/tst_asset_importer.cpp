#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include "core/assets/AssetLibrary.h"
#include "core/mcp/BlenderGate.h"
#include "core/unity/AssetImporter.h"

class AssetImporterTest : public QObject
{
    Q_OBJECT
private slots:
    // 转换脚本回归：绝不能再出现 read_factory_settings（会断 Blender 连接），
    // 必须定向清空 + 带贴图导出（避免白模）
    void conversionScriptAvoidsFactoryReset()
    {
        const QString script = AssetImporter::glbToFBXScript("/a/crate.glb", "/a/crate.fbx");
        QVERIFY2(!script.contains(QStringLiteral("read_factory_settings")),
                 "转换脚本不得用 read_factory_settings（会重置偏好、关掉 BlenderMCP 插件→断连）");
        QVERIFY(script.contains(QStringLiteral("bpy.ops.object.delete")));  // 定向清空
        QVERIFY(script.contains(QStringLiteral("embed_textures=True")));    // 带贴图，防白模
        QVERIFY(script.contains("/a/crate.glb"));
        QVERIFY(script.contains("/a/crate.fbx"));
    }

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

    // 回执超时/桥抖动时命令可能仍在 Blender 里排队执行，fbx 会迟到落地：
    // 只要宽限窗口内出现，就必须算成功，不能被误报成转换失败。
    void waitsForLateFbx()
    {
        QTemporaryDir dir;
        const QString fbx = dir.path() + "/crate.fbx";
        QTimer::singleShot(300, [fbx] {
            QFile file(fbx);
            if (file.open(QIODevice::WriteOnly))
                file.write("FBX");
        });
        bool finished = false;
        bool ok = false;
        AssetImporter::waitForFile(fbx, 3000, [&](bool appeared) {
            ok = appeared;
            finished = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY(ok);
    }

    // 真失败：文件始终不出现时，要等满窗口再判死，而不是零延迟误杀
    void givesUpWhenFbxNeverAppears()
    {
        QTemporaryDir dir;
        const QString fbx = dir.path() + "/never.fbx";
        bool finished = false;
        bool ok = true;
        QElapsedTimer elapsed;
        elapsed.start();
        AssetImporter::waitForFile(fbx, 600, [&](bool appeared) {
            ok = appeared;
            finished = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY(!ok);
        QVERIFY2(elapsed.elapsed() >= 550, "必须等满宽限窗口，不能一看到没文件就判失败");
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
