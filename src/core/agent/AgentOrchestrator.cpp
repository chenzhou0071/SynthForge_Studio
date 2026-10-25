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

namespace {

const char* kPlanSystemPrompt =
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

int planBudget(int groupCount)
{
    // 一组通常要对着 5~7 个物体反复建/截图/修正，一次工具调用不算一步，但一轮对话算一步：
    // 30/组留足重试余量（4 组 120 步），封顶 200 防止计划器拆组过多时无限膨胀。
    return qMin(30 * groupCount, 200);
}

BuildPlan parsePlan(const QString& text)
{
    const int start = text.indexOf('{');
    const int end = text.lastIndexOf('}');
    if (start < 0 || end <= start)
        return {}; // 空计划 = 解析失败
    return buildPlanFromJson(
        QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8()).object());
}

BuildPlan defaultPlan()
{
    BuildPlan plan;
    plan.groups = {
        {QStringLiteral("底座环境"), {}, {}, QStringLiteral("俯视截图：底座完整、路面/标线齐全、无悬空凹陷")},
        {QStringLiteral("建筑外观"), {}, {}, QStringLiteral("街角视角截图：主体、招牌、门窗齐全，比例协调")},
        {QStringLiteral("店内陈设"), {}, {}, QStringLiteral("透过橱窗截图：按提示词店内清单逐项核对")},
        {QStringLiteral("灯光特效"), {}, {}, QStringLiteral("夜景截图：招牌自发光、路灯、雨水粒子均已生效")},
    };
    plan.maxSteps = planBudget(plan.groups.size());
    return plan;
}

QString blenderSystemPrompt()
{
    return QStringLiteral(
        "你是 SynthForge Studio 的 Blender 建模 agent。使用提供的 MCP 工具"
        "（execute_blender_code 等）完成低模场景：先清场再分组建造，"
        "必要时截图查看结果并修正；全部完成后按规范导出并落盘。\n"
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
        "布局纪律（逐条执行，不得违反）：\n"
        "4. 所有物体落地：object.location.z = 0，禁止悬空或埋地。\n"
        "5. 建每个新物体前，先用 execute_blender_code 遍历 bpy.data.objects，"
        "读出已有物体的 location 与 dimensions；新物体的水平投影不得与它们交叠。\n"
        "6. 场景中心在原点，主建筑正面朝 -Y，人行道沿 X 轴展开。\n"
        "7. 常用尺寸（米）：树 4~6，路灯 5，路牌 2.5，门 2.1，楼层 3，人 1.75。\n"
        "8. 底座边长按真实尺度反推：必须放得下主体完整立面、入口前至少 2 米人行道"
        "与近景道具；不得为了压缩画面把主体、门窗或道具做小。\n"
        "9. 不得添加与主题无关的物体；贴合主题的合理补充（车辆、路牌、摊贩等）"
        "可作为独立物体加入，并在说明里交代。\n"
        "10. 灯光必须是真实光源对象（AREA/POINT/SPOT/SUN），室内每个空间至少一盏；"
        "不要用自发光平面、光雾、飞虫、湿地面或贴图冒充照明。光粒子、光雾可以用粒子/着色器"
        "增强氛围，但必须有真实光源支撑；不要默认生成飞虫、湿地面、积水等元素，用户明确要求时"
        "才做，且不得用几何体冒充。");
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
    for (const QJsonValue& rule : json.value(QStringLiteral("rules")).toArray())
        plan.rules << rule.toString();
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
    plan.maxSteps = totalSteps > 0 ? totalSteps : planBudget(plan.groups.size());
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
                             const QString& area)
{
    m_taskId = taskId;
    m_tm = tm;
    m_prompt = prompt;
    m_area = area;
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
    const QJsonArray messages =
        initialMessages(QString::fromUtf8(kPlanSystemPrompt), m_prompt);
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
                 BuildPlan plan = parsePlan(m_stream);
                 if (plan.groups.isEmpty() && attempt < 1) {
                     attemptPlan(attempt + 1); // 解析失败重试 1 次
                     return;
                 }
                 if (plan.groups.isEmpty())
                     plan = defaultPlan(); // 仍失败 → 降级默认分组
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
                                            {"rules", QJsonArray::fromStringList(plan.rules)},
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
    m_maxSteps = plan.maxSteps > 0 ? plan.maxSteps : 50;
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
        blenderSystemPrompt(),
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
    // 用户显式授权继续：跟主预算同档（30/组），加上收尾余量，单次最多 200 步
    const int remaining = qMax(1, m_plan.groups.size() - m_groupIndex);
    m_maxSteps += qMin(30 * remaining + 20, 200);
    m_stream.clear();
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
        content += QStringLiteral("\n只做一次自检：最多截图 1 次，只对照验收标准检查；发现问题才修，"
                                  "修完最多再截 1 次确认。不要逐项复述物体坐标、间隙或贴图状态；"
                                  "确认无误就回一句“自检通过”并立即停止调用工具；不要复述本条要求。");
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
        m_mcp->callTool(name, args,
                        [this, call, name](const QJsonValue& result, const QJsonObject& error) {
                            if (m_toolCallHook)
                                m_toolCallHook(name, false);
                            emit toolStep(m_area, name, error.isEmpty() ? "done" : "error");
                            QStringList images;
                            m_messages.append(
                                toolResultMessage(call.value(QStringLiteral("id")).toString(),
                                                  result, error, &images));
                            for (const QString& dataUrl : images)
                                emit imageReceived(m_area, m_taskId, dataUrl, name);
                            if (--m_pendingToolCalls == 0) {
                                m_awaitingTools = false;
                                m_stream.clear();
                                tick(); // 回喂后继续下一轮
                            }
                        });
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
