#include "core/agent/AgentOrchestrator.h"

#include "core/llm/ToolBridge.h"
#include "core/mcp/BlenderGate.h"
#include "core/mcp/McpClient.h"
#include "core/tasks/TaskManager.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>

namespace {

// 取模型输出里的 JSON 对象；没有花括号就当解析失败（返回空对象）
QJsonObject planJsonFrom(const QString& text)
{
    const int start = text.indexOf('{');
    const int end = text.lastIndexOf('}');
    if (start < 0 || end <= start)
        return {};
    return QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8()).object();
}

// 各档的兜底分组：规划器不吐 JSON、或开放场景拿不出主轴分区时用它。
// 盒式舞台沿用分档前的四组与逐字相同的验收文案，行为不能变。
BuildPlan defaultPlan(OutputShape shape)
{
    BuildPlan plan;
    plan.shape = shape;
    switch (shape) {
    case OutputShape::Prop:
        plan.groups = {
            {QStringLiteral("主体"), {}, {}, QStringLiteral("正视截图：主体完整、各部件落地不悬空")},
            {QStringLiteral("附属小件"), {}, {}, QStringLiteral("俯视截图：小件齐全且各自是独立物体")},
        };
        plan.rules = {QStringLiteral("不要底座与地面：资产之外的空处留空")};
        break;
    case OutputShape::OpenScene:
        plan.groups = {
            {QStringLiteral("地面与主轴"), {}, {}, QStringLiteral("沿主轴截图：主轴贯通、长度达标")},
            {QStringLiteral("近景分区"), {}, {}, QStringLiteral("近景截图：入口与细节可见")},
            {QStringLiteral("中景分区"), {}, {}, QStringLiteral("中景截图：主体构成完整")},
            {QStringLiteral("远景分区"), {}, {}, QStringLiteral("朝远处截图：有纵深，不是空到天际")},
        };
        plan.zones = {QStringLiteral("主轴：主路径或轨道沿 +Y 方向延伸"),
                      QStringLiteral("近景：0~20 米 入口与主体近处"),
                      QStringLiteral("中景：20~60 米 主体区"),
                      QStringLiteral("远景：60 米以上 纵深与剪影")};
        plan.rules = {QStringLiteral("沿主轴展开，禁止把物体都堆在原点一圈"),
                      QStringLiteral("远景分区必须有内容，不许用空地代替")};
        break;
    case OutputShape::Diorama:
        break;
    }
    if (plan.groups.isEmpty()) { // 盒式舞台沿用现状四组
        plan.groups = {
            {QStringLiteral("底座环境"), {}, {}, QStringLiteral("俯视截图：底座完整、路面/标线齐全、无悬空凹陷")},
            {QStringLiteral("建筑外观"), {}, {}, QStringLiteral("街角视角截图：主体、招牌、门窗齐全，比例协调")},
            {QStringLiteral("店内陈设"), {}, {}, QStringLiteral("透过橱窗截图：按提示词店内清单逐项核对")},
            {QStringLiteral("灯光特效"), {}, {}, QStringLiteral("夜景截图：招牌自发光、路灯、雨水粒子均已生效")},
        };
    }
    plan.maxSteps = shapeBudget(plan.shape, plan.groups.size());
    return plan;
}

// 开放场景没有主轴与分区就等于「一堆楼随机撒开」：判为不合格，交给重试与降级
bool planAcceptable(const BuildPlan& plan)
{
    if (plan.groups.isEmpty())
        return false;
    if (plan.shape == OutputShape::OpenScene && plan.zones.size() < 2)
        return false;
    return true;
}

QString blenderSystemPrompt(OutputShape shape)
{
    // 通用层只留「三档都成立」的条款：朝向、底座边长这类微缩舞台专属纪律
    // 已经搬进 shapeDiscipline，留在通用层会把道具档框回方盒子。
    const QString body = QStringLiteral(
        "你是 SynthForge Studio 的 Blender 建模 agent。使用提供的 MCP 工具"
        "（execute_blender_code 等）完成低模建造：先清场再分组建造，"
        "必要时截图查看结果并修正。不要自行导出或保存文件："
        "导出与保存由系统在最后的导出回合统一执行。\n"
        "沟通方式（必须遵守）：全程用简体中文（对象名、代码、文件路径除外），"
        "不要用英文写进度说明或最终回复。每一轮先用 1 句中文说明这一步要做什么、"
        "依据是什么，再调用工具，不要只发工具调用不解释；工具返回截图后，"
        "只看关键问题，用中文说明是否达标、下一步怎么改，不要复述截图里的细节。"
        "中间轮次不要写验收报告：每组建造完成时最多 2 句（做了什么、是否完成），"
        "不要列坐标、顶点误差、BVH/包围盒、材质或动作通道等测量数据；"
        "不要复述系统消息或用户消息里的指令原文，例如“按提示词要求”“按你给的脚本”。\n"
        "硬性约束（Unity 导入与跨工具复用依赖这三条，不得违反）：\n"
        "1. 场景单位为米：1 Blender 单位 = 1 米（门高约 2.1，楼层约 3，人眼高 1.65）。\n"
        "2. 每个道具的 origin 落在包围盒底部中心，摆到平面上不悬空。\n"
        "3. 贴图必须打包进 .blend（导出前 pack_all），不得引用外部图片。\n"
        "通用布局纪律（逐条执行，不得违反）：\n"
        "4. 所有物体落地：object.location.z = 0，禁止悬空或埋地。\n"
        "5. 建每个新物体前，先用 execute_blender_code 遍历 bpy.data.objects，"
        "读出已有物体的 location 与 dimensions；新物体的水平投影不得与它们交叠。\n"
        "6. 常用尺寸（米）：树 4~6，路灯 5，路牌 2.5，门 2.1，楼层 3，人 1.75。\n"
        "7. 不得添加与主题无关的物体；贴合主题的合理补充（车辆、路牌、摊贩等）"
        "可作为独立物体加入，并在说明里交代。\n"
        "8. 灯光必须是真实光源对象（AREA/POINT/SPOT/SUN），室内每个空间至少一盏；"
        "不要用自发光平面、光雾、飞虫、湿地面或贴图冒充照明。光粒子、光雾可以用粒子/着色器"
        "增强氛围，但必须有真实光源支撑；不要默认生成飞虫、湿地面、积水等元素，用户明确要求时"
        "才做，且不得用几何体冒充。\n"
        "9. 建造与自检阶段禁止调用 export_scene / save_as_mainfile；"
        "收到系统的导出指令后再执行，且只执行一次。");
    // 分档纪律接在最后：编号与通用条独立，避免改一处要重排全部编号
    return body + shapeDiscipline(shape);
}

// 截图为 image content 时压成 JPEG base64：同一份结果既回喂模型又给前端展示
QByteArray compressImage(const QByteArray& png)
{
    QImage image;
    if (!image.loadFromData(png))
        return {};
    if (image.width() > 1024 || image.height() > 1024)
        image = image.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return out;
}

QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error, QStringList* images)
{
    QJsonArray content;
    if (!error.isEmpty()) {
        content.append(QJsonObject{
            {"type", "text"},
            {"text", "ERROR: " + QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact))}});
    } else {
        for (const QJsonValue& item : result.toObject().value(QStringLiteral("content")).toArray()) {
            const QJsonObject obj = item.toObject();
            if (obj.value(QStringLiteral("type")).toString() == QLatin1String("image")) {
                const QByteArray jpeg = compressImage(
                    QByteArray::fromBase64(obj.value(QStringLiteral("data")).toString().toUtf8()));
                if (jpeg.isEmpty())
                    continue;
                const QString dataUrl =
                    QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64());
                content.append(QJsonObject{{"type", "image_url"},
                                           {"image_url", QJsonObject{{"url", dataUrl}}}});
                if (images)
                    images->append(dataUrl);
            } else {
                content.append(obj);
            }
        }
    }
    return QJsonObject{{"role", "tool"}, {"tool_call_id", callId}, {"content", content}};
}

} // namespace

// 规划器输出与 planReady 下发用同一套字段名；plan.confirm 靠它还原成可执行计划
BuildPlan buildPlanFromJson(const QJsonObject& json)
{
    BuildPlan plan;
    // 旧 JSON（分档前落库的历史行）没有 shape：shapeFromString 回落盒式舞台
    plan.shape = shapeFromString(json.value(QStringLiteral("shape")).toString());
    for (const QJsonValue& rule : json.value(QStringLiteral("rules")).toArray())
        plan.rules << rule.toString();
    for (const QJsonValue& zone : json.value(QStringLiteral("zones")).toArray())
        plan.zones << zone.toString();
    for (const QJsonValue& motion : json.value(QStringLiteral("motions")).toArray())
        plan.motions << motion.toString();
    for (const QJsonValue& value : json.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject item = value.toObject();
        BuildGroup group;
        group.title = item.value(QStringLiteral("title")).toString();
        for (const QJsonValue& name : item.value(QStringLiteral("items")).toArray())
            group.items << name.toString();
        for (const QJsonValue& entry : item.value(QStringLiteral("placements")).toArray()) {
            const QJsonObject entryObj = entry.toObject();
            BuildPlacement placement;
            placement.name = entryObj.value(QStringLiteral("name")).toString();
            placement.where = entryObj.value(QStringLiteral("where")).toString();
            if (!placement.name.isEmpty() && !placement.where.isEmpty())
                group.placements.push_back(placement);
        }
        group.acceptance = item.value(QStringLiteral("acceptance")).toString();
        if (!group.title.isEmpty())
            plan.groups.push_back(group);
    }
    const int totalSteps = json.value(QStringLiteral("totalSteps")).toInt(); // 规划阶段已封顶
    plan.maxSteps = totalSteps > 0 ? totalSteps : shapeBudget(plan.shape, plan.groups.size());
    return plan;
}

AgentOrchestrator::AgentOrchestrator(LlmGateway* llm, McpClient* mcp, QObject* parent)
    : QObject(parent)
    , m_llm(llm)
    , m_mcp(mcp)
{
}

void AgentOrchestrator::setToolCallHook(std::function<void(const QString&, bool)> hook)
{
    m_toolCallHook = std::move(hook);
}

void AgentOrchestrator::setConversationHistory(const QJsonArray& history)
{
    m_history = history;
}

void AgentOrchestrator::plan(const QString& taskId, TaskManager* tm, const QString& prompt,
                             const QString& area, OutputShape shape)
{
    m_taskId = taskId;
    m_tm = tm;
    m_prompt = prompt;
    m_area = area;
    m_shape = shape;
    m_resumable = false; // 新指令作废旧的续跑现场
    m_planning.insert(area);
    attemptPlan(0);
}

void AgentOrchestrator::attemptPlan(int attempt)
{
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    const QJsonArray messages = initialMessages(planSystemPromptFor(m_shape), m_prompt);
    m_stream.clear();
    m_llm->chatStream(
        m_cfg, messages, {},
        {.onDelta =
             [this](const QString& d) {
                 m_stream += d;
             },
         .onToolCalls = nullptr,
         .onError = [this](const QString& e) {
             fail(e);
         },
         .onDone =
             [this, attempt]() {
                 const QJsonObject json = planJsonFrom(m_stream);
                 BuildPlan plan = buildPlanFromJson(json);
                 // 档位以本次任务为准：模型自己写了个 shape 也不能把这一单改成别的档
                 plan.shape = m_shape;
                 // 预算跟着档位走：模型没给 totalSteps 就必须按档重算（道具 20/组）
                 if (json.value(QStringLiteral("totalSteps")).toInt() <= 0)
                     plan.maxSteps = shapeBudget(m_shape, plan.groups.size());
                 if (!planAcceptable(plan) && attempt < 1) {
                     attemptPlan(attempt + 1); // 解析失败重试 1 次
                     return;
                 }
                 if (!planAcceptable(plan))
                     plan = defaultPlan(m_shape); // 仍不合格 → 降级该档默认分组
                 QJsonArray groups;
                 for (const BuildGroup& g : plan.groups) {
                     QJsonArray placements;
                     for (const BuildPlacement& p : g.placements)
                         placements.append(QJsonObject{{"name", p.name}, {"where", p.where}});
                     groups.append(QJsonObject{
                         {"title", g.title},
                         {"items", QJsonArray::fromStringList(g.items)},
                         {"placements", placements},
                         {"acceptance", g.acceptance},
                     });
                 }
                 m_planning.remove(m_area);
                 emit planReady(m_area, m_taskId,
                                QJsonObject{{"taskId", m_taskId},
                                            {"shape", shapeKey(plan.shape)},
                                            {"rules", QJsonArray::fromStringList(plan.rules)},
                                            {"zones", QJsonArray::fromStringList(plan.zones)},
                                            {"motions", QJsonArray::fromStringList(plan.motions)},
                                            {"groups", groups},
                                            {"totalSteps", plan.maxSteps}});
             }});
}

void AgentOrchestrator::execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools,
                                const BuildPlan& plan, const QString& prompt,
                                const QString& targetFolder, const QString& area)
{
    m_taskId = taskId;
    m_tm = tm;
    m_tools = tools;
    m_plan = plan;
    m_area = area;
    // 建造阶段沿用规划时的档位：纪律、自检与预算都从这张表读
    m_shape = plan.shape;
    m_maxSteps = plan.maxSteps > 0 ? plan.maxSteps : shapeBudget(m_shape, m_plan.groups.size());
    m_steps = 0;
    m_groupIndex = 0;
    m_phase = GroupPhase::Build;
    m_targetFolder = targetFolder;
    m_startedAt = QDateTime::currentDateTime();
    m_exporting = false;
    m_resumable = false;
    if (m_plan.groups.isEmpty()) {
        fail(QStringLiteral("empty-plan"));
        return;
    }
    if (m_gate && m_area == QLatin1String("blender")) {
        if (!m_gate->tryAcquire(QStringLiteral("blender-agent"))) {
            fail(QStringLiteral("blender-busy")); // 不排队：调用方提示用户稍后重试
            return;
        }
        m_gateHeld = true;
    }
    m_messages = initialMessages(
        blenderSystemPrompt(m_shape),
        QStringLiteral("已确认的专业提示词：\n%1\n目标存放目录：%2。").arg(prompt, targetFolder));

    // 清场：活会话里还留着上一次任务的模型，不清就会被 save_as_mainfile 一起存进新 blend
    // （先做超市再做路灯，路灯.blend 里就会带着超市）。确定性操作直调 MCP，不交给 LLM 记得。
    static const QString kClearScene = QStringLiteral(R"python(
import bpy
# 不用 read_factory_settings：那会重置 preferences，可能把 BlenderMCP 插件关掉、连接直接断
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images,
             bpy.data.armatures, bpy.data.actions, bpy.data.cameras, bpy.data.lights):
    for item in list(coll):
        if item.users == 0:
            coll.remove(item)
)python");
    if (!m_mcp) { // 单测/降级路径：没有 MCP 就没有会话可清
        startGroup();
        return;
    }
    m_mcp->callTool(QStringLiteral("execute_blender_code"), QJsonObject{{"code", kClearScene}},
                    [this](const QJsonValue&, const QJsonObject& error) {
                        if (m_tm && m_tm->isCancelled(m_taskId)) {
                            finishCancelled();
                            return;
                        }
                        if (!error.isEmpty()) {
                            fail(QStringLiteral("clear-scene-failed"));
                            return;
                        }
                        startGroup();
                    });
}

void AgentOrchestrator::resume(const QString& taskId, TaskManager* tm)
{
    if (!m_resumable || m_plan.groups.isEmpty()) {
        if (tm)
            tm->finish(taskId); // 没现场可续：别把任务挂在队列里
        return;
    }
    m_taskId = taskId;
    m_tm = tm;
    if (m_gate && m_area == QLatin1String("blender")) {
        if (!m_gate->tryAcquire(QStringLiteral("blender-agent"))) {
            fail(QStringLiteral("blender-busy")); // 忙：保留续跑资格，稍后可再点
            return;
        }
        m_gateHeld = true;
    }
    m_resumable = false;
    // 用户显式授权继续：跟主预算同档（按档位单组步数），加上收尾余量，单次最多 200 步
    const int perGroup = shapePolicy(m_shape).budgetPerGroup;
    const int remaining = qMax(1, m_plan.groups.size() - m_groupIndex);
    m_maxSteps += qMin(perGroup * remaining + 20, 200);
    m_stream.clear();
    if (m_exporting) // 上次卡在导出回合：续跑时前端仍要挂“导出中”
        emit exportStarted(m_area, m_targetFolder);
    tick(); // 不清场、不重发分组指令：接着上一轮的会话往下跑
}

QJsonArray AgentOrchestrator::initialMessages(const QString& system, const QString& user) const
{
    QJsonArray messages{QJsonObject{{"role", "system"}, {"content", system}}};
    for (const QJsonValue& item : m_history)
        messages.append(item); // 规格 8.4：本区最近 20 条纯文本历史
    messages.append(QJsonObject{{"role", "user"}, {"content", user}});
    return messages;
}

void AgentOrchestrator::releaseGate()
{
    if (m_gateHeld && m_gate)
        m_gate->release(QStringLiteral("blender-agent"));
    m_gateHeld = false;
}

void AgentOrchestrator::finishCancelled()
{
    m_planning.remove(m_area);
    m_resumable = false;
    releaseGate();
    if (m_tm)
        m_tm->finish(m_taskId); // TaskManager 已记录取消，finished(cancelled=true)
    m_taskId.clear();
}

void AgentOrchestrator::fail(const QString& reason)
{
    m_planning.remove(m_area);
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    if (m_taskId.isEmpty()) {
        releaseGate(); // 防御：无任务 id 也不遗留租约
        return;
    }
    const QString normalized = reason == QLatin1String("max steps exceeded")
                                   ? QStringLiteral("max-steps-exceeded")
                                   : reason;
    // 只有步数耗尽值得续跑；busy 是暂时的，不清掉已攒下的续跑资格
    m_resumable = normalized == QLatin1String("max-steps-exceeded")
                  || (m_resumable && normalized == QLatin1String("blender-busy"));
    emit failed(m_area, normalized);
    releaseGate();
    if (m_tm)
        m_tm->finish(m_taskId);
    m_taskId.clear();
}

void AgentOrchestrator::startGroup()
{
    const BuildGroup& group = m_plan.groups[m_groupIndex];
    emit groupProgress(m_area, m_groupIndex + 1, m_plan.groups.size(), group.title);
    QString content =
        QStringLiteral("开始第 %1/%2 组「%3」。本组对象：%4。")
            .arg(m_groupIndex + 1)
            .arg(m_plan.groups.size())
            .arg(group.title, group.items.isEmpty() ? QStringLiteral("按提示词清单执行")
                                                    : group.items.join(QStringLiteral("、")));
    if (!group.placements.isEmpty()) {
        QStringList lines;
        for (const BuildPlacement& p : group.placements)
            lines << QStringLiteral("- %1：%2").arg(p.name, p.where);
        content +=
            QStringLiteral("\n摆放要求（相对关系优先，具体坐标自行根据已有物体的包围盒推算）：\n%1")
                .arg(lines.join('\n'));
    }
    // 开放场景的主轴分区与动线只有规划器知道，不逐组复述模型就只能猜
    if (!m_plan.zones.isEmpty()) {
        content += QStringLiteral("\n本场景的空间结构（必须遵守，不得把物体堆回原点一圈）：\n- %1")
                       .arg(m_plan.zones.join(QStringLiteral("\n- ")));
    }
    if (!m_plan.motions.isEmpty()) {
        content +=
            QStringLiteral("\n时序与运动（涉及本组物体时按此实现，位移关键帧必须做到路径终点）：\n- %1")
                .arg(m_plan.motions.join(QStringLiteral("\n- ")));
    }
    content += QStringLiteral("\n本档产出形态：%1。").arg(shapeLabel(m_shape));
    content += QStringLiteral("\n放置前先查询场景中已有物体的位置与尺寸，避免重叠。"
                              "完成后停止调用工具；本组结束只回 1 句结果，不写验收报告。");
    m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
    m_stream.clear();
    tick();
}

void AgentOrchestrator::finishTurnText()
{
    // 文字只属于当前这一轮：先收口再进入工具/下一组，避免下一轮的话续到上一个气泡
    const QString text = m_stream;
    m_stream.clear();
    if (!text.trimmed().isEmpty())
        emit turnFinished(m_area, text);
}

void AgentOrchestrator::onTurnFinished() // 一轮 LLM 无工具调用时进入
{
    if (m_phase == GroupPhase::Build) {
        finishTurnText();
        m_phase = GroupPhase::SelfCheck;
        const BuildGroup& group = m_plan.groups[m_groupIndex];
        QString content = QStringLiteral("第 %1 组建造结束。请对照验收标准：%2。")
                              .arg(m_groupIndex + 1)
                              .arg(group.acceptance);
        if (!m_plan.rules.isEmpty())
            content +=
                QStringLiteral("\n本任务布局约束（逐条核对，不满足则修正后再结束）：\n- %1")
                    .arg(m_plan.rules.join(QStringLiteral("\n- ")));
        // 自检怎么截几张图、截几次按档位读表；收尾这句三档共用，所以次数交给表、
        // 这里只钉「一趟体检」的上限（写「最多两轮」等于给盒式舞台偷偷放宽）
        content += QStringLiteral("\n%1\n"
                                  "只体检一次：按上面的截图次数检查，发现问题才修，修完确认一次即可。"
                                  "不要逐项复述物体坐标、间隙或贴图状态；"
                                  "确认无误就回一句“自检通过”并立即停止调用工具；不要复述本条要求。")
                       .arg(shapeSelfCheck(m_shape));
        m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
        m_stream.clear();
        tick();
        return;
    }
    if (m_groupIndex + 1 < m_plan.groups.size()) {
        finishTurnText();
        ++m_groupIndex;
        m_phase = GroupPhase::Build;
        startGroup();
        return;
    }
    if (!m_exporting) {
        finishTurnText();
        m_exporting = true; // 最后一轮：按规格 5.5 导出三件套并保存源文件
        emit exportStarted(m_area, m_targetFolder);
        // 修改任务已有目标基名：直接覆盖，不能让“撞名加序号”把修改变成新建
        const bool overwrite = !m_plan.targetBase.isEmpty();
        const QString baseHint =
            overwrite ? QStringLiteral(" \"%1\"（覆盖同名文件，不要加序号）").arg(m_plan.targetBase)
                      : QStringLiteral("草稿命名清单里的英文基名");
        QString prompt =
            QStringLiteral("全部组已完成。不要再做场景体检、不要截图，也不要重复前面已确认的结论；"
                           "现在执行导出（execute_blender_code），把下面脚本原样执行，"
                           "只把 <BASENAME> 换成%1，不要改逻辑：\n"
                           "import bpy, os\n"
                           "folder = r\"%2\"\n"
                           "name = \"<BASENAME>\"\n")
                .arg(baseHint, m_targetFolder);
        if (overwrite) {
            prompt += QStringLiteral("base = name\n");
        } else {
            prompt += QStringLiteral("base, n = name, 1\n"
                                     "while os.path.exists(os.path.join(folder, base + \".blend\")):\n"
                                     "    n += 1\n"
                                     "    base = name + \"_\" + str(n)\n");
        }
        prompt += QStringLiteral(
            "bpy.ops.export_scene.gltf(filepath=os.path.join(folder, base + \".glb\"),\n"
            "                          export_format='GLB', export_animations=True,\n"
            "                          export_animation_mode='ACTIONS', export_frame_range=True)\n"
            "bpy.ops.export_scene.fbx(filepath=os.path.join(folder, base + \".fbx\"))\n"
            "bpy.ops.file.pack_all()\n"
            "prefs = bpy.context.preferences.filepaths\n"
            "old_save_version = prefs.save_version\n"
            "prefs.save_version = 0\n"
            "try:\n"
            "    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(folder, base + \".blend\"))\n"
            "finally:\n"
            "    prefs.save_version = old_save_version\n"
            "print('EXPORTED_BASE=' + base)\n");
        prompt += overwrite
                      ? QStringLiteral("不要删除场景里已有的关键帧动画（导出时它们会作为 GLB 动画保留）。"
                                       "三个文件必须同基名并覆盖原文件；本次任务只执行一次导出，"
                                       "不要改名或再次保存；导出完成后停止调用工具。")
                      : QStringLiteral(
                            "不要删除场景里已有的关键帧动画（导出时它们会作为 GLB 动画保留）。"
                            "三个文件必须同基名；如果 base 和 name 不同（加了序号），"
                            "在最终回复里告诉用户实际文件名；"
                            "本次任务只执行一次导出，不要改名或再次保存；导出完成后停止调用工具。");
        // 收尾只留结论与清单，别把工具日志/警告/文件大小全复述一遍
        prompt += QStringLiteral(
            "最终回复保持简短：先给一句结论，再用最多 5 条清单列出改了什么或产出了什么；"
            "不要复述本条要求或脚本内容；"
            "不要罗列文件大小、节点/材质数量、工具日志、警告或备份文件；"
            "只有失败、改名或需要用户处理时才补充一句说明。");
        m_messages.append(QJsonObject{{"role", "user"}, {"content", prompt}});
        m_stream.clear();
        tick();
        return;
    }
    finishOk(m_stream);
}

void AgentOrchestrator::tick()
{
    if (m_tm && m_tm->isCancelled(m_taskId)) {
        finishCancelled();
        return;
    }
    if (++m_steps > m_maxSteps) {
        fail(QStringLiteral("max steps exceeded"));
        return;
    }
    m_llm->chatStream(
        m_cfg, m_messages, m_tools,
        {.onDelta =
             [this](const QString& d) {
                 m_stream += d;
                 emit delta(m_area, d);
             },
         .onToolCalls = [this](const QJsonArray& calls) { executeToolCalls(calls); },
         .onError = [this](const QString& e) { fail(e); },
         .onDone =
             [this]() {
                 if (m_pendingToolCalls == 0 && !m_awaitingTools)
                     onTurnFinished();
             }},
        m_area); // tag = 区名，chat.cancel 才能真正断掉在途流
}

void AgentOrchestrator::executeToolCalls(const QJsonArray& calls)
{
    m_awaitingTools = true;
    m_pendingToolCalls = calls.size();
    // 先说话再干活：工具调用前的交代单独收口，截图回来后的下一段另起气泡
    const QString turnText = m_stream;
    finishTurnText();
    // calls 是 LlmGateway 的内部格式 {id,name,arguments(object)}；回灌历史必须转回 OpenAI wire format
    m_messages.append(QJsonObject{{"role", "assistant"},
                                  {"content", turnText},
                                  {"tool_calls", ToolBridge::internalCallsToWire(calls)}});
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QString name = call.value(QStringLiteral("name")).toString();
        const QJsonObject args = call.value(QStringLiteral("arguments")).toObject();
        emit toolStep(m_area, name, "running");
        if (m_toolCallHook)
            m_toolCallHook(name, true); // P6：异步工具调用全程抑制 watcher
        auto finishCall = [this, call, name](const QJsonValue& result,
                                             const QJsonObject& error) {
            if (m_toolCallHook)
                m_toolCallHook(name, false);
            emit toolStep(m_area, name, error.isEmpty() ? "done" : "error");
            QStringList images;
            m_messages.append(toolResultMessage(call.value(QStringLiteral("id")).toString(),
                                                result, error, &images));
            for (const QString& dataUrl : images)
                emit imageReceived(m_area, m_taskId, dataUrl, name);
            if (--m_pendingToolCalls == 0) {
                m_awaitingTools = false;
                m_stream.clear();
                tick(); // 回喂后继续下一轮
            }
        };

        // 建造/自检阶段模型自己 export/save 会先生成一份文件，系统收尾再导出就变成 _2。
        // 这里硬拦：只有 m_exporting 的导出回合允许落盘。
        const QString code = args.value(QStringLiteral("code")).toString();
        if (name == QLatin1String("execute_blender_code") && !m_exporting
            && (code.contains(QLatin1String("export_scene"))
                || code.contains(QLatin1String("save_as_mainfile")))) {
            // 必须等本轮 LLM 的 onDone 先到：同步回包会让 onDone 误以为工具已结束，
            // 把建造阶段直接推到自检，跳过用户脚本里的下一轮文字。
            QTimer::singleShot(0, this, [finishCall]() {
                finishCall({}, QJsonObject{{"message",
                                            QStringLiteral("建造阶段禁止自行导出或保存；"
                                                           "导出由系统在最后统一执行，请继续建模。")}});
            });
            continue;
        }

        m_mcp->callTool(name, args, finishCall);
    }
}

void AgentOrchestrator::finishOk(const QString& text)
{
    m_resumable = false;
    const QStringList glbs = collectNewGlbs();
    if (!glbs.isEmpty())
        emit artifactsReady(m_area, glbs); // 先报产出，再收尾对话
    emit finalText(m_area, text);
    releaseGate(); // 先释放门禁，再推进同区队列
    if (m_tm)
        m_tm->finish(m_taskId);
    m_taskId.clear();
}

QStringList AgentOrchestrator::collectNewGlbs() const
{
    QStringList result;
    if (m_targetFolder.isEmpty())
        return result;
    const QDir dir(m_targetFolder);
    const QDateTime threshold = m_startedAt.addSecs(-2); // 容忍文件系统时间精度
    for (const QFileInfo& info : dir.entryInfoList({"*.glb"}, QDir::Files)) {
        if (info.lastModified() >= threshold)
            result << info.absoluteFilePath();
    }
    return result;
}
