#include "core/agent/ShapePolicy.h"

namespace {

const char* kPropKey = "prop";
const char* kDioramaKey = "diorama";
const char* kOpenKey = "open";

// 三份规划器提示词共用一段「JSON 规范」说明，只在组数与施工顺序上分叉：
// 复制三整份以后必然改一处漏两处。
// 正文逐字沿用分档前的 kPlanSystemPrompt（含那几处便利店味的举例）：
// 盒式舞台这一档的硬要求是「逐字不变」，丢一个字就是偷偷改了旧链路，
// tst_shape_policy 的黄金基线测试会立刻红。举例只是格式示范，不会把火车站拽成商店，
// 因此不为另外两档另写举例。
QString planJsonSpec(int minGroups, int maxGroups, const QString& order)
{
    return QStringLiteral(
               "你是低模场景建造的规划器。只输出一个 JSON 对象，不要任何解释或代码块围栏，格式："
               "{\"rules\":[\"本任务特有的跨组约束\"],\"groups\":[{\"title\":\"分组名\","
               "\"items\":[\"物体1\"],"
               "\"placements\":[{\"name\":\"物体1\",\"where\":\"相对位置与朝向\"}],"
               "\"acceptance\":\"自检时截图检查什么\"}]}。"
               "要求：%1~%2 组，覆盖用户提示词中全部必须独立的物体与数量下限；按施工顺序排列"
               "（%3）。"
               "可以在主题内合理补充道具（车辆、路牌、摊贩等），但不得添加与主题无关的物体，"
               "也不得为凑组数拆出杂物组；留白优先于堆砌。"
               "items 里的每个物体必须在 placements 里有一条同名条目；"
               "where 只写相对关系不写绝对坐标"
               "（如「商店北侧边缘，正门朝 -Y」「招牌在正门上方，离地 2.8 米」）。"
               "rules 只写本任务特有、跨组生效的约束（如「树不遮挡招牌」「路灯每 8 米一盏」），"
               "不要重复贴地、不重叠、单位这类通用规则。")
        .arg(minGroups)
        .arg(maxGroups)
        .arg(order);
}

} // namespace

OutputShape shapeFromString(const QString& text)
{
    if (text == QLatin1String(kPropKey))
        return OutputShape::Prop;
    if (text == QLatin1String(kOpenKey))
        return OutputShape::OpenScene;
    return OutputShape::Diorama; // 未知值一律回落：ui.ini 被手改坏也不能顶崩界面
}

QString shapeKey(OutputShape shape)
{
    switch (shape) {
    case OutputShape::Prop:
        return QLatin1String(kPropKey);
    case OutputShape::OpenScene:
        return QLatin1String(kOpenKey);
    case OutputShape::Diorama:
        break;
    }
    return QLatin1String(kDioramaKey);
}

QString shapeLabel(OutputShape shape)
{
    switch (shape) {
    case OutputShape::Prop:
        return QStringLiteral("道具");
    case OutputShape::OpenScene:
        return QStringLiteral("开放场景");
    case OutputShape::Diorama:
        break;
    }
    return QStringLiteral("盒式舞台");
}

const ShapePolicy& shapePolicy(OutputShape shape)
{
    static const ShapePolicy kProp{QStringLiteral(":/prompts/summarizer-prop.md"),
                                   QStringLiteral(":/prompts/ask-prop.md"), 20, 40, 1, 2};
    static const ShapePolicy kDiorama{QStringLiteral(":/prompts/summarizer.md"),
                                      QStringLiteral(":/prompts/ask.md"), 30, 200, 3, 6};
    static const ShapePolicy kOpen{QStringLiteral(":/prompts/summarizer-open.md"),
                                   QStringLiteral(":/prompts/ask-open.md"), 40, 320, 3, 8};
    switch (shape) {
    case OutputShape::Prop:
        return kProp;
    case OutputShape::OpenScene:
        return kOpen;
    case OutputShape::Diorama:
        break;
    }
    return kDiorama;
}

int shapeBudget(OutputShape shape, int groupCount)
{
    const ShapePolicy& policy = shapePolicy(shape);
    // 一轮对话算一步、一次工具调用不算一步：道具一次成型，舞台要反复截图修，
    // 开放场景对象多且按机位/按帧多次截图，所以单组预算与总封顶都更高。
    return qMin(policy.budgetPerGroup * qMax(1, groupCount), policy.budgetCap);
}

QString planSystemPromptFor(OutputShape shape)
{
    const ShapePolicy& policy = shapePolicy(shape);
    switch (shape) {
    case OutputShape::Prop:
        return planJsonSpec(policy.minGroups, policy.maxGroups,
                            QStringLiteral("主体 → 附属小件，超过 2 组就是拆得太碎"));
    case OutputShape::OpenScene:
        return planJsonSpec(policy.minGroups, policy.maxGroups,
                            QStringLiteral(
                                "地面与主轴 → 近景分区 → 中景分区 → 远景分区，"
                                "同一分区的物体放在同一组"));
    case OutputShape::Diorama:
        break;
    }
    return planJsonSpec(policy.minGroups, policy.maxGroups,
                        QStringLiteral("底座环境 → 建筑外观 → 室内陈设 → 灯光特效"));
}

QString shapeDiscipline(OutputShape shape)
{
    switch (shape) {
    case OutputShape::Prop:
        return QStringLiteral(
            "道具档纪律（本档专用，逐条执行）：\n"
            "P1. 不要任何底座、地面、地平面对象：资产外的空处就是空处。\n"
            "P2. 所有物体挂在一个根空对象下，根对象名 = 资产英文名，"
            "位于世界原点；每个子物体自己的 origin 落在其底部中心。\n"
            "P3. 不要做雾、粒子、光雾、飞虫、湿地面等氛围特效。\n"
            "P4. 只允许属于物体本身的功能光源（台灯、屏幕、灯管），"
            "不要太阳光、环境光、世界环境设置。\n"
            "P5. 子物体不得合并成一个网格：进 Unity 后要能单独取用。");
    case OutputShape::OpenScene:
        return QStringLiteral(
            "开放场景档纪律（本档专用，逐条执行）：\n"
            "O1. 不设方形边界：沿规划给出的主轴展开，近景 / 中景 / 远景三段都要有内容，"
            "禁止把物体都堆在原点一圈。\n"
            "O2. 长条结构（轨道、道路、河沿、围墙）按重复段搭建，"
            "总长度必须达到主轴要求，不得为了省事在中途截断。\n"
            "O3. 氛围层（雾、体积光、粒子）照做，但必须放进名为 FX 的集合里"
            "（FX 集合不会进入 glb/fbx，只在 .blend 里保留，人在 Blender 看效果）。\n"
            "O4. 有运动物体时，关键帧位移必须完整覆盖到路径终点："
            "不能因为「反正被雾挡住」就只做到中途，导出后 FX 不存在。\n"
            "O5. 运动物体的位移 / 旋转必须有独立 action（首尾一致以便无缝循环），"
            "不要用 driver 或约束驱动（约束请先烘焙成关键帧）。");
    case OutputShape::Diorama:
        break;
    }
    return QStringLiteral(
        "盒式舞台档纪律（本档专用，逐条执行）：\n"
        "D1. 地面是一个完整的正方形底座，所有元素都落在底座内，不得伸出边缘。\n"
        "D2. 底座边长按真实尺度反推：放得下主体完整立面、入口前至少 2 米人行道"
        "与近景道具；不得为了压缩画面把主体、门窗或道具做小。\n"
        "D3. 构图紧凑、有近 / 中 / 远三层纵深，呈现小巧精致的模型感。\n"
        "D4. 氛围层（雾、体积光、粒子）照做，但必须放进名为 FX 的集合里"
        "（FX 集合不会进入 glb/fbx，只在 .blend 里保留）。");
}

QString shapeSelfCheck(OutputShape shape)
{
    switch (shape) {
    case OutputShape::Prop:
        return QStringLiteral(
            "道具自检：截 3 张——正视、俯视、侧视。核对无底座无地面、"
            "子物体各自落地不悬空、根空对象在原点。");
    case OutputShape::OpenScene:
        return QStringLiteral(
            "开放场景自检：沿主轴截 3 张——近景、中景、远景各一次，"
            "确认三段都有内容且主轴没被杂物堵死；"
            "若本组有运动物体，再按起始帧 / 中间帧 / 结束帧各截 1 次"
            "（先用 execute_blender_code 设 bpy.context.scene.frame_set(帧号) 再截图），"
            "确认运动物体在路径上而不是穿模或原地不动。");
    case OutputShape::Diorama:
        break;
    }
    return QStringLiteral(
        "盒式舞台自检：截图 1 次，只对照验收标准检查；发现问题才修，"
        "修完最多再截 1 次确认。");
}
