#include "core/agent/DiscussionAgent.h"

#include "core/agent/AskReply.h"
#include "core/llm/LlmGateway.h"
#include "core/llm/ToolBridge.h"
#include "core/llm/ToolMessage.h"
#include "core/mcp/McpClient.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <memory>

namespace {

// 讨论里的工具调用上限：一次问答的「看」不该超过 6 次，
// 否则模型会没完没了地截图确认，用户等一分钟出不来一句话。
constexpr int kMaxAskToolCalls = 6;

const QStringList kToolWhitelist{QStringLiteral("get_scene_info"), QStringLiteral("look")};

QString inspectSchema()
{
    return QStringLiteral(
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"要查看的 Blender 对象名\"}},"
        "\"required\":[\"name\"]}");
}

QString pyString(const QString& text)
{
    QString out;
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case u'\\':
            out += QLatin1String("\\\\");
            break;
        case u'\'':
            out += QLatin1String("\\'");
            break;
        case u'\n':
            out += QLatin1String("\\n");
            break;
        case u'\r':
            out += QLatin1String("\\r");
            break;
        case u'\t':
            out += QLatin1String("\\t");
            break;
        default:
            out += ch;
        }
    }
    return out;
}

} // namespace

bool isSafeObjectName(const QString& name)
{
    // 名字会被塞进 Python 字面量：白名单字符之外一律拒（含分号、引号、控制符、空、超长）
    if (name.isEmpty() || name.size() > 64)
        return false;
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.\\-\\p{Han}]+$"));
    return pattern.match(name).hasMatch();
}

QString inspectObjectScript(const QString& name)
{
    return QStringLiteral(
               "import bpy, json\n"
               "name = '%1'\n"
               "ob = bpy.data.objects.get(name)\n"
               "if ob is None:\n"
               "    print(json.dumps({'error': 'object-not-found',\n"
               "                      'candidates': sorted(o.name for o in bpy.data.objects)[:40]},\n"
               "                     ensure_ascii=False))\n"
               "else:\n"
               "    action = None\n"
               "    if ob.animation_data and ob.animation_data.action:\n"
               "        action = ob.animation_data.action.name\n"
               "    info = {\n"
               "        'name': ob.name,\n"
               "        'type': ob.type,\n"
               "        'parent': ob.parent.name if ob.parent else None,\n"
               "        'children': [c.name for c in ob.children][:40],\n"
               "        'location': [round(v, 3) for v in ob.location],\n"
               "        'rotation_euler': [round(v, 3) for v in ob.rotation_euler],\n"
               "        'scale': [round(v, 3) for v in ob.scale],\n"
               "        'dimensions': [round(v, 3) for v in ob.dimensions],\n"
               "        'materials': [m.name for m in ob.data.materials] if hasattr(ob.data, 'materials') else [],\n"
               "        'modifiers': [m.name for m in ob.modifiers],\n"
               "        'action': action,\n"
               "    }\n"
               "    print(json.dumps(info, ensure_ascii=False))\n")
        .arg(pyString(name));
}

QJsonArray discussionTools(const QJsonArray& mcpTools)
{
    const QJsonArray allowed = ToolBridge::mcpToolsToOpenAi(mcpTools);
    QJsonArray kept;
    for (const QJsonValue& value : allowed) {
        const QString name =
            value.toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name"))
                .toString();
        if (kToolWhitelist.contains(name))
            kept.append(value);
    }
    // 虚拟工具：接口只有「看」的语义，实现走后端固定脚本
    kept.append(QJsonObject{{"type", "function"},
                            {"function", QJsonObject{{"name", "inspect_object"},
                                                     {"description",
                                                      "读取指定 Blender 对象的尺寸、材质、修改器、"
                                                      "父子关系与动画状态（只读）"},
                                                     {"parameters",
                                                      QJsonDocument::fromJson(
                                                          inspectSchema().toUtf8())
                                                          .object()}}}});
    return kept;
}

DiscussionAgent::DiscussionAgent(LlmGateway* llm, McpClient* mcp, QObject* parent)
    : QObject(parent)
    , m_llm(llm)
    , m_mcp(mcp)
{
}

void DiscussionAgent::ask(const QString& area, const QString& systemPrompt,
                          const QJsonArray& history, const QString& userText, bool withTools)
{
    m_area = area;
    m_stream.clear();
    m_visible.clear();
    m_toolCalls = 0;
    m_inFlight = 0;
    m_roundDone = false;
    // 就绪与否由调用方（CoreApp）体现在 withTools 上，这里不再判 m_mcp->isReady()
    m_tools = withTools && m_mcp ? discussionTools(m_mcpTools) : QJsonArray{};
    m_messages = QJsonArray{QJsonObject{{"role", "system"}, {"content", systemPrompt}}};
    for (const QJsonValue& item : history)
        m_messages.append(item);
    m_messages.append(QJsonObject{{"role", "user"}, {"content", userText}});
    streamOnce();
}

void DiscussionAgent::streamOnce()
{
    auto filter = std::make_shared<AskStreamFilter>();
    LlmGateway::StreamHandlers handlers;
    handlers.onDelta = [this, filter](const QString& piece) {
        m_stream += piece;
        const QString show = filter->feed(piece);
        if (!show.isEmpty()) {
            m_visible += show;
            emit delta(m_area, show);
        }
    };
    handlers.onToolCalls = [this](const QJsonArray& calls) { executeCalls(calls); };
    handlers.onError = [this](const QString& error) { emit failed(m_area, error); };
    handlers.onDone = [this, filter]() {
        if (m_inFlight > 0 || m_roundDone)
            return; // 工具在途：等最后一个结果回来再继续；已收尾：不双发
        const QString tail = filter->finish();
        if (!tail.isEmpty()) {
            m_visible += tail;
            emit delta(m_area, tail);
        }
        finishRound();
    };
    m_llm->chatStream(m_cfg, m_messages, m_tools, handlers, m_area);
}

void DiscussionAgent::finishRound()
{
    if (m_roundDone)
        return;
    m_roundDone = true;
    const AskReply reply = parseAskReply(m_stream);
    // 落库/广播的正文用累加的 m_visible（含跨工具轮已展示的字），并裁掉尾部空白：
    // 与改造前 parseAskReply().visible 的 .trimmed() 行为一致，刷新后内容不会多出换行
    const QString shown = m_visible.isEmpty() ? reply.visible : m_visible.trimmed();
    emit finished(m_area, shown, reply.executable);
}

void DiscussionAgent::executeCalls(const QJsonArray& calls)
{
    if (m_toolCalls >= kMaxAskToolCalls) {
        // 预算用完：这一轮当作「无工具」收尾，不再回喂，避免无限看图
        const QString note = QStringLiteral("（查看次数已达上限，以下结论基于已看到的内容。）");
        m_visible += note;
        emit delta(m_area, note);
        finishRound();
        return;
    }
    // 回喂历史必须转回 OpenAI wire format（与 AgentOrchestrator 同规则）
    m_messages.append(QJsonObject{{"role", "assistant"},
                                  {"content", m_stream},
                                  {"tool_calls", ToolBridge::internalCallsToWire(calls)}});
    m_inFlight = calls.size();
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QString id = call.value(QStringLiteral("id")).toString();
        const QString name = call.value(QStringLiteral("name")).toString();
        const QJsonObject args = call.value(QStringLiteral("arguments")).toObject();
        ++m_toolCalls;
        emit toolStep(m_area, name, "running");

        QString mcpName = name;
        QJsonObject payload = args;
        if (name == QLatin1String("inspect_object")) {
            // 虚拟工具：接口只有「看」的语义，脚本由后端固定拼，名字不安全就直接拒
            const QString objectName = args.value(QStringLiteral("name")).toString();
            mcpName = QStringLiteral("execute_blender_code");
            payload = QJsonObject{
                {"code", isSafeObjectName(objectName)
                             ? inspectObjectScript(objectName)
                             : QStringLiteral("print('{\"error\":\"unsafe-object-name\"}')")}};
        } else if (!kToolWhitelist.contains(name)) {
            // 模型硬编了一个白名单外的工具：本地拒绝，不转发给 MCP
            emit toolStep(m_area, name, "error");
            m_messages.append(QJsonObject{{"role", "tool"},
                                          {"tool_call_id", id},
                                          {"content", QJsonArray{QJsonObject{
                                              {"type", "text"},
                                              {"text", QStringLiteral("ERROR: 讨论模式只允许 "
                                                                      "get_scene_info / look / "
                                                                      "inspect_object")}}}}});
            if (--m_inFlight == 0) {
                m_stream.clear();
                streamOnce();
            }
            continue;
        }

        m_mcp->callTool(mcpName, payload, [this, id, name](const QJsonValue& result,
                                                           const QJsonObject& error) {
            emit toolStep(m_area, name, error.isEmpty() ? "done" : "error");
            QStringList images;
            m_messages.append(toolResultMessage(id, result, error, &images));
            for (const QString& dataUrl : images)
                emit imageReceived(m_area, dataUrl, name);
            if (--m_inFlight == 0) {
                m_stream.clear();
                streamOnce(); // 结果全部回喂后继续同一轮讨论
            }
        });
    }
}
