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

    void propPolicyForbidsBaseplateAndAmbience()
    {
        // 道具是「可复用单体资产」：底座、地面、氛围特效都会让它进 Unity 后没法用
        const QString discipline = shapeDiscipline(OutputShape::Prop);
        QVERIFY(discipline.contains(QStringLiteral("不要任何底座、地面、地平面对象")));
        QVERIFY(discipline.contains(QStringLiteral("不要做雾、粒子、光雾")));
        QVERIFY(discipline.contains(QStringLiteral("根空对象")));
        QVERIFY(!shapeSelfCheck(OutputShape::Prop).contains(QStringLiteral("正方形")));
    }

    void openScenePolicyDemandsAxisAndZones()
    {
        const QString discipline = shapeDiscipline(OutputShape::OpenScene);
        QVERIFY(discipline.contains(QStringLiteral("主轴")));
        QVERIFY(discipline.contains(QStringLiteral("FX")));
        // 氛围层只留在 .blend：导出剔掉后如果位移不到位，Unity 里火车会停在空地中间
        QVERIFY(discipline.contains(QStringLiteral("关键帧位移必须完整覆盖到路径终点")));
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
