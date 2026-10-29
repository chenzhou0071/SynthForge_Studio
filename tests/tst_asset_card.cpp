#include <QtTest>

#include "core/coreapp/AssetCard.h"

#include <QJsonArray>

class AssetCardTest : public QObject
{
    Q_OBJECT
private slots:
    void summaryUsesFirstBodyLineAndCutsAtTwenty()
    {
        const QString prompt = QStringLiteral(
            "主题与视角：一把木质餐椅，四条腿略微外八，靠背有横向木条。\n\n"
            "底座与构图：无底座。\n");

        QCOMPARE(promptSummary(prompt),
                 QStringLiteral("一把木质餐椅，四条腿略微外八，靠背有横向…"));
    }

    void summarySkipsHeadingsAndSectionLabels()
    {
        const QString prompt = QStringLiteral(
            "## 本次任务\n\n- 用户需求：做一把椅子\n\n细节内容：座面有软垫。\n");

        QCOMPARE(promptSummary(prompt), QStringLiteral("做一把椅子"));
    }

    void cardListsFilesInExportOrderAndOmitsPolyCount()
    {
        AssetCardInput input;
        input.name = QStringLiteral("chair_01");
        input.source = QStringLiteral("lowpoly");
        input.prompt = QStringLiteral("主题与视角：一把椅子。");
        input.paths = QJsonObject{{QStringLiteral("blend"), QStringLiteral("props/chair_01.blend")},
                                  {QStringLiteral("glb"), QStringLiteral("props/chair_01.glb")},
                                  {QStringLiteral("fbx"), QStringLiteral("props/chair_01.fbx")}};
        input.dir = QStringLiteral("props");
        input.createdAt = QStringLiteral("2026-10-29 14:00");

        const QJsonObject card = makeAssetCard(input);

        QCOMPARE(card.value(QStringLiteral("name")).toString(), QStringLiteral("chair_01"));
        QCOMPARE(card.value(QStringLiteral("source")).toString(), QStringLiteral("lowpoly"));
        const QJsonArray files = card.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 3);
        QCOMPARE(files.at(0).toObject().value(QStringLiteral("suffix")).toString(),
                 QStringLiteral("glb"));
        QCOMPARE(files.at(2).toObject().value(QStringLiteral("suffix")).toString(),
                 QStringLiteral("blend"));
        QVERIFY(!card.contains(QStringLiteral("faces")));
        QVERIFY(!card.contains(QStringLiteral("polygons")));
        QVERIFY(!card.contains(QStringLiteral("triangles")));
    }

    void cloudCardCarriesProviderAndModel()
    {
        AssetCardInput input;
        input.name = QStringLiteral("crate");
        input.source = QStringLiteral("cloud3d");
        input.provider = QStringLiteral("tripo");
        input.model = QStringLiteral("v3.1-20260211");
        input.prompt = QStringLiteral("主题与视角：一只板条箱。");
        input.paths = QJsonObject{{QStringLiteral("glb"), QStringLiteral("crate.glb")}};

        const QJsonObject card = makeAssetCard(input);

        QCOMPARE(card.value(QStringLiteral("provider")).toString(), QStringLiteral("tripo"));
        QCOMPARE(card.value(QStringLiteral("model")).toString(),
                 QStringLiteral("v3.1-20260211"));
        QCOMPARE(card.value(QStringLiteral("files")).toArray().size(), 1);
    }

    // 讨论模式靠资产卡知道这是哪一档，否则改书桌时还在按微缩场景的口吻聊
    void cardCarriesShapeForDiscussionToPickTemplate()
    {
        AssetCardInput input;
        input.name = QStringLiteral("desk");
        input.source = QStringLiteral("lowpoly");
        input.shape = QStringLiteral("prop");
        input.prompt = QStringLiteral("主体与用途：一张实木书桌");
        input.dir = QStringLiteral("Props");
        input.createdAt = QStringLiteral("2026-10-29");
        input.paths = QJsonObject{{QStringLiteral("glb"), QStringLiteral("d:/desk.glb")},
                                  {QStringLiteral("blend"), QStringLiteral("d:/desk.blend")}};

        const QJsonObject card = makeAssetCard(input);

        QCOMPARE(card.value(QStringLiteral("shape")).toString(), QStringLiteral("prop"));
        QCOMPARE(card.value(QStringLiteral("files")).toArray().size(), 2);
    }

    // 缺省必须写成 diorama：旧资产、旧任务没有档位，留空会让前端自己猜
    void emptyShapeMeansDiorama()
    {
        AssetCardInput input;
        input.name = QStringLiteral("shop");
        input.source = QStringLiteral("lowpoly");
        input.prompt = QStringLiteral("主题与视角：雨夜便利店");

        const QJsonObject card = makeAssetCard(input);

        QCOMPARE(card.value(QStringLiteral("shape")).toString(), QStringLiteral("diorama"));
    }
};

QTEST_MAIN(AssetCardTest)
#include "tst_asset_card.moc"
