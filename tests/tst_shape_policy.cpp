#include <QtTest>

#include "core/agent/ShapePolicy.h"

class ShapePolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void unknownKeyFallsBackToDiorama()
    {
        QCOMPARE(shapeFromString(QStringLiteral("prop")), OutputShape::Prop);
        QCOMPARE(shapeFromString(QStringLiteral("open")), OutputShape::OpenScene);
        QCOMPARE(shapeFromString(QStringLiteral("cinema")), OutputShape::Diorama);
        QCOMPARE(shapeFromString(QString()), OutputShape::Diorama);
        // 键名必须能被往返：写进 ui.ini / plan JSON 再读回来不能变档
        QCOMPARE(shapeKey(shapeFromString(QStringLiteral("open"))), QStringLiteral("open"));
    }

    void labelsAreChineseAndDistinct()
    {
        QCOMPARE(shapeLabel(OutputShape::Prop), QStringLiteral("道具"));
        QCOMPARE(shapeLabel(OutputShape::Diorama), QStringLiteral("盒式舞台"));
        QCOMPARE(shapeLabel(OutputShape::OpenScene), QStringLiteral("开放场景"));
    }

    void templatesArePerShapeAndExistInResources()
    {
        QCOMPARE(shapePolicy(OutputShape::Prop).summarizeTemplate,
                 QStringLiteral(":/prompts/summarizer-prop.md"));
        QCOMPARE(shapePolicy(OutputShape::Diorama).summarizeTemplate,
                 QStringLiteral(":/prompts/summarizer.md"));
        QCOMPARE(shapePolicy(OutputShape::OpenScene).askTemplate,
                 QStringLiteral(":/prompts/ask-open.md"));
    }

    void budgetDiffersByShapeAndIsCapped()
    {
        // 道具单组就够，预算小；开放场景对象多、截图多次，预算大但封顶
        QCOMPARE(shapeBudget(OutputShape::Prop, 1), 20);
        QCOMPARE(shapeBudget(OutputShape::Prop, 9), 40);   // 20×9=180 → 封顶 40
        QCOMPARE(shapeBudget(OutputShape::Diorama, 4), 120); // 现状不变
        QCOMPARE(shapeBudget(OutputShape::Diorama, 9), 200);
        QCOMPARE(shapeBudget(OutputShape::OpenScene, 5), 200);
        QCOMPARE(shapeBudget(OutputShape::OpenScene, 9), 320);
    }

    // 三档纪律逐条钉住。Task 1 只挑了三句代表去断言，剩下的条款删掉也不会红，
    // 而漏钉的 O2/O5 恰恰是进 Unity 最先踩的坑（轨道修半截、动画带不进去）。
    // 加纪律必须在这里加一行，否则它就是一段没人的裸文本。
    void disciplinePinsEveryClause_data()
    {
        QTest::addColumn<QString>("shapeKey");
        QTest::addColumn<QString>("phrase");
        // 道具：可复用单体资产——底座、氛围特效、合并成一片网格都会让它没法用
        QTest::newRow("prop-no-baseplate")
            << QStringLiteral("prop") << QStringLiteral("不要任何底座、地面、地平面对象");
        QTest::newRow("prop-root-empty")
            << QStringLiteral("prop") << QStringLiteral("所有物体挂在一个根空对象下");
        QTest::newRow("prop-no-ambience")
            << QStringLiteral("prop") << QStringLiteral("不要做雾、粒子、光雾、飞虫、湿地面等氛围特效");
        QTest::newRow("prop-functional-lights-only")
            << QStringLiteral("prop") << QStringLiteral("只允许属于物体本身的功能光源");
        QTest::newRow("prop-keep-children-split")
            << QStringLiteral("prop") << QStringLiteral("子物体不得合并成一个网格");
        // 盒式舞台：D2 底座边长与 D5 朝向是从旧通用层搬进来的，丢了就是退化
        QTest::newRow("diorama-square-baseplate")
            << QStringLiteral("diorama") << QStringLiteral("地面是一个完整的正方形底座");
        QTest::newRow("diorama-baseplate-scale")
            << QStringLiteral("diorama") << QStringLiteral("底座边长按真实尺度反推");
        QTest::newRow("diorama-three-depth-layers")
            << QStringLiteral("diorama") << QStringLiteral("有近 / 中 / 远三层纵深");
        QTest::newRow("diorama-fx-collection")
            << QStringLiteral("diorama") << QStringLiteral("必须放进名为 FX 的集合里");
        QTest::newRow("diorama-orientation")
            << QStringLiteral("diorama") << QStringLiteral("主建筑正面朝 -Y");
        // 开放场景：沿主轴展开、长条不许截断、动画要能带进 Unity
        QTest::newRow("open-no-square-boundary")
            << QStringLiteral("open") << QStringLiteral("不设方形边界");
        QTest::newRow("open-repeat-segments")
            << QStringLiteral("open") << QStringLiteral("按重复段搭建");
        QTest::newRow("open-no-truncation")
            << QStringLiteral("open") << QStringLiteral("不得为了省事在中途截断");
        QTest::newRow("open-fx-collection")
            << QStringLiteral("open") << QStringLiteral("必须放进名为 FX 的集合里");
        // 氛围层只留在 .blend：导出剔掉后位移不到位的话，火车会停在空地中间
        QTest::newRow("open-motion-covers-path")
            << QStringLiteral("open") << QStringLiteral("关键帧位移必须完整覆盖到路径终点");
        // 独立 action 且首尾一致才能无缝循环；driver / 约束导不出来，只剩静态模型
        QTest::newRow("open-independent-action")
            << QStringLiteral("open") << QStringLiteral("有独立 action（首尾一致以便无缝循环）");
        QTest::newRow("open-no-driver")
            << QStringLiteral("open") << QStringLiteral("不要用 driver 或约束驱动");
    }

    void disciplinePinsEveryClause()
    {
        QFETCH(QString, shapeKey);
        QFETCH(QString, phrase);
        QVERIFY2(shapeDiscipline(shapeFromString(shapeKey)).contains(phrase),
                 qPrintable(QStringLiteral("%1 档纪律缺条款：%2").arg(shapeKey, phrase)));
    }

    // 分档边界：别档的专属条款不许串档。道具写「中途截断」「driver」会和
    // 「一次成型、无动画」自相矛盾；开放场景写「正方形底座」就把它框回了方盒子。
    void disciplineKeepsForeignClausesOut_data()
    {
        QTest::addColumn<QString>("shapeKey");
        QTest::addColumn<QString>("forbidden");
        QTest::newRow("prop-no-baseplate-rule")
            << QStringLiteral("prop") << QStringLiteral("正方形底座");
        QTest::newRow("prop-no-orientation-rule")
            << QStringLiteral("prop") << QStringLiteral("主建筑正面朝 -Y");
        QTest::newRow("prop-no-truncation-rule")
            << QStringLiteral("prop") << QStringLiteral("中途截断");
        QTest::newRow("prop-no-animation-rule")
            << QStringLiteral("prop") << QStringLiteral("driver");
        QTest::newRow("open-no-baseplate-rule")
            << QStringLiteral("open") << QStringLiteral("正方形底座");
        QTest::newRow("open-no-baseplate-scale")
            << QStringLiteral("open") << QStringLiteral("底座边长");
        QTest::newRow("open-no-root-empty-rule")
            << QStringLiteral("open") << QStringLiteral("根空对象");
        // 盒式舞台沿用改造前的通用 + 分档文本，这两句本来就没有，搬进来等于改了旧链路
        QTest::newRow("diorama-no-root-empty-rule")
            << QStringLiteral("diorama") << QStringLiteral("根空对象");
        QTest::newRow("diorama-no-truncation-rule")
            << QStringLiteral("diorama") << QStringLiteral("中途截断");
    }

    void disciplineKeepsForeignClausesOut()
    {
        QFETCH(QString, shapeKey);
        QFETCH(QString, forbidden);
        QVERIFY2(!shapeDiscipline(shapeFromString(shapeKey)).contains(forbidden),
                 qPrintable(QStringLiteral("%1 档混进了别档的条款：%2").arg(shapeKey, forbidden)));
    }

    // 自检策略同样逐条钉：截几张、截哪几帧决定模型能不能发现自己建错了
    void selfCheckPinsEveryClause_data()
    {
        QTest::addColumn<QString>("shapeKey");
        QTest::addColumn<QString>("phrase");
        QTest::newRow("prop-three-views") << QStringLiteral("prop") << QStringLiteral("截 3 张");
        QTest::newRow("prop-front-view") << QStringLiteral("prop") << QStringLiteral("正视");
        QTest::newRow("diorama-one-shot")
            << QStringLiteral("diorama") << QStringLiteral("截图 1 次");
        QTest::newRow("diorama-fix-then-confirm")
            << QStringLiteral("diorama") << QStringLiteral("修完最多再截 1 次确认");
        QTest::newRow("open-three-depths")
            << QStringLiteral("open") << QStringLiteral("近景、中景、远景各一次");
        QTest::newRow("open-frame-set") << QStringLiteral("open") << QStringLiteral("frame_set");
        QTest::newRow("open-three-frames")
            << QStringLiteral("open") << QStringLiteral("起始帧 / 中间帧 / 结束帧");
    }

    void selfCheckPinsEveryClause()
    {
        QFETCH(QString, shapeKey);
        QFETCH(QString, phrase);
        QVERIFY2(shapeSelfCheck(shapeFromString(shapeKey)).contains(phrase),
                 qPrintable(QStringLiteral("%1 档自检缺内容：%2").arg(shapeKey, phrase)));
    }

    void selfCheckKeepsForeignClausesOut_data()
    {
        QTest::addColumn<QString>("shapeKey");
        QTest::addColumn<QString>("forbidden");
        QTest::newRow("prop-no-square") << QStringLiteral("prop") << QStringLiteral("正方形");
        QTest::newRow("prop-no-frames") << QStringLiteral("prop") << QStringLiteral("frame_set");
        // 盒式舞台没有动画，不该被要求按帧截图
        QTest::newRow("diorama-no-frames") << QStringLiteral("diorama") << QStringLiteral("frame_set");
        // 「截图 1 次」是盒式舞台的上限，套到动画档就等于永远发现不了穿模
        QTest::newRow("open-no-single-shot-cap") << QStringLiteral("open") << QStringLiteral("截图 1 次");
    }

    void selfCheckKeepsForeignClausesOut()
    {
        QFETCH(QString, shapeKey);
        QFETCH(QString, forbidden);
        QVERIFY2(!shapeSelfCheck(shapeFromString(shapeKey)).contains(forbidden),
                 qPrintable(QStringLiteral("%1 档自检混进了别档内容：%2").arg(shapeKey, forbidden)));
    }

    void planPromptsAreNotTheSameText()
    {
        QVERIFY(planSystemPromptFor(OutputShape::Prop)
                != planSystemPromptFor(OutputShape::OpenScene));
        // 分档边界：只有盒式舞台的规划器该提"底座"；道具一提就会长出莫名其妙的地板，
        // 开放场景该沿主轴展开而不是被底座框住
        QVERIFY(planSystemPromptFor(OutputShape::Diorama).contains(QStringLiteral("底座")));
        QVERIFY(!planSystemPromptFor(OutputShape::Prop).contains(QStringLiteral("底座")));
        QVERIFY(!planSystemPromptFor(OutputShape::OpenScene).contains(QStringLiteral("底座")));
        QVERIFY(planSystemPromptFor(OutputShape::OpenScene).contains(QStringLiteral("主轴")));
    }

    // 黄金基线：盒式舞台这一档必须逐字不变。常量是从分档前
    // AgentOrchestrator.cpp 的 kPlanSystemPrompt 原样抄下来的，
    // 共用正文里丢一个字（哪怕只是举例的括号）这条就红。
    void dioramaPlanPromptIsVerbatimLegacyText()
    {
        static const char* kLegacyPlanSystemPrompt =
            "你是低模场景建造的规划器。只输出一个 JSON 对象，不要任何解释或代码块围栏，格式："
            "{\"rules\":[\"本任务特有的跨组约束\"],\"groups\":[{\"title\":\"分组名\","
            "\"items\":[\"物体1\"],"
            "\"placements\":[{\"name\":\"物体1\",\"where\":\"相对位置与朝向\"}],"
            "\"acceptance\":\"自检时截图检查什么\"}]}。"
            "要求：3~6 组，覆盖用户提示词中全部必须独立的物体与数量下限；按施工顺序排列"
            "（底座环境 → 建筑外观 → 室内陈设 → 灯光特效）。"
            "可以在主题内合理补充道具（车辆、路牌、摊贩等），但不得添加与主题无关的物体，"
            "也不得为凑组数拆出杂物组；留白优先于堆砌。"
            "items 里的每个物体必须在 placements 里有一条同名条目；where 只写相对关系不写绝对坐标"
            "（如「商店北侧边缘，正门朝 -Y」「招牌在正门上方，离地 2.8 米」）。"
            "rules 只写本任务特有、跨组生效的约束（如「树不遮挡招牌」「路灯每 8 米一盏」），"
            "不要重复贴地、不重叠、单位这类通用规则。";
        QCOMPARE(planSystemPromptFor(OutputShape::Diorama),
                 QString::fromUtf8(kLegacyPlanSystemPrompt));
    }
};

QTEST_GUILESS_MAIN(ShapePolicyTest)
#include "tst_shape_policy.moc"
