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
};

QTEST_GUILESS_MAIN(ShapePolicyTest)
#include "tst_shape_policy.moc"
