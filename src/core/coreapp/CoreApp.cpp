#include "core/coreapp/CoreApp.h"

#include "core/agent/AgentOrchestrator.h"
#include "core/agent/AskReply.h"
#include "core/agent/ShapePolicy.h"
#include "core/agent/SummarizerAgent.h"
#include "core/assets/AssetLibrary.h"
#include "core/cloud3d/Cloud3dService.h"
#include "core/coreapp/AssetCard.h"
#include "core/llm/LlmGateway.h"
#include "core/llm/ToolBridge.h"
#include "core/mcp/BridgeCommands.h"
#include "core/mcp/McpRuntime.h"
#include "core/mcp/StdioTransport.h"
#include "core/preflight/PreflightChecker.h"
#include "core/settings/SettingsStore.h"
#include "core/settings/UiState.h"
#include "core/store/SessionStore.h"
#include "core/tasks/TaskManager.h"
#include "core/ws/ProtocolRouter.h"
#include "core/ws/WsServer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>
#include <QUrl>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

CoreApp::CoreApp(const CoreOptions& options, QObject* parent)
    : QObject(parent)
    , m_options(options)
    , m_settings(std::make_unique<SettingsStore>(options.dataDir))
    , m_uiState(std::make_unique<UiState>(options.dataDir))
    , m_sessions(
          std::make_unique<SessionStore>(options.dataDir + QStringLiteral("/sessions.db")))
    , m_tasks(std::make_unique<TaskManager>())
    , m_router(std::make_unique<ProtocolRouter>())
    , m_ws(std::make_unique<WsServer>(m_router.get()))
    , m_llm(std::make_unique<LlmGateway>())
{
    // 页面晚于桥接加载时，ready 事件已经过去：新连接一律补发当前状态
    connect(m_ws.get(), &WsServer::clientConnected, this,
            &CoreApp::broadcastStatusSnapshot);
}

CoreApp::~CoreApp() = default;

bool CoreApp::start()
{
    m_settings->load();
    if (!m_sessions->open())
        return false;

    // 资产库根目录：Documents/SynthForgeStudio/ModelLibrary（规格 5.5）
    const QString libraryRoot =
        m_options.libraryRoot.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                  + QStringLiteral("/SynthForgeStudio/ModelLibrary")
            : m_options.libraryRoot;
    QDir().mkpath(libraryRoot);
    m_library = std::make_unique<AssetLibrary>(libraryRoot);

    // Blender 桥接运行时：先建好再注册 handler，保证 chat.send 的前置检查有可用对象
    const BridgeCommand bridge =
        m_options.blenderBridge.program.isEmpty()
            ? defaultBridgeCommand(QStringLiteral("blender"))
            : m_options.blenderBridge;
    m_blender = new McpRuntime(QStringLiteral("blender"),
                               std::make_unique<StdioTransport>(bridge), this);
    connect(m_blender, &McpRuntime::toolsChanged, this, [this](const QJsonArray& tools) {
        m_blenderTools = tools;
        if (m_blender->isReady()) // 顶栏悬停文案：已就绪 · N 个工具（规格 4.2）
            broadcastConnStatus(QStringLiteral("blender"), QStringLiteral("ready"),
                                QStringLiteral("已就绪 · %1 个工具").arg(tools.size()));
    });
    connect(m_blender, &McpRuntime::statusChanged, this, &CoreApp::broadcastConnStatus);
    registerBridge(QStringLiteral("blender"), [this] { return m_blender->restart(); });
    m_preflight = std::make_unique<PreflightChecker>(m_settings.get(), m_blender);

    m_summarizer = new SummarizerAgent(m_llm.get(), m_sessions.get(), m_settings.get(), this);
    connect(m_summarizer, &SummarizerAgent::draftReady, this,
            [this](const QString& area, const QString& prompt) {
                const QString asset = m_pendingDraftAsset.take(area);
                const QString source = m_pendingDraftSource.take(area);
                const QString pendingMode = m_pendingDraftMode.take(area);
                // 重新生成：同一条草稿原地换文本，不新增卡片
                if (const qint64 regenId = m_regenDraft.take(area); regenId > 0) {
                    m_sessions->updateMessageContent(regenId, prompt);
                    m_ws->broadcastEvent(
                        QStringLiteral("draft.updated"),
                        QJsonObject{{"area", area}, {"id", regenId}, {"prompt", prompt}});
                    return;
                }
                const QString mode = pendingMode.isEmpty()
                    ? (asset.isEmpty() ? QStringLiteral("lowpoly")
                                       : QStringLiteral("modify"))
                    : pendingMode;
                const qint64 id = m_sessions->appendMessage(
                    area, QStringLiteral("draft"), prompt,
                    QJsonObject{{"state", "pending"}, {"origin", "generated"},
                                {"mode", mode}, {"asset", asset}, {"source", source}});
                m_ws->broadcastEvent(
                    QStringLiteral("draft.created"),
                    QJsonObject{{"area", area}, {"id", id}, {"prompt", prompt},
                                {"origin", "generated"}, {"mode", mode}, {"asset", asset}});
            });
    // 云 3D：服务自己轮询/下载，CoreApp 只做事件转发
    m_cloud = std::make_unique<Cloud3dService>(m_settings.get(), m_library.get());
    connect(m_cloud.get(), &Cloud3dService::progress, this,
            [this](const QString& taskId, const QString& state, int percent) {
                m_ws->broadcastEvent(QStringLiteral("task.progress"),
                                     QJsonObject{{"area", "blender"},
                                                 {"taskId", taskId},
                                                 {"state", state},
                                                 {"percent", percent}});
            });
    connect(m_cloud.get(), &Cloud3dService::errorCard, this,
            [this](const QString& reason, const QString& message) {
                QJsonObject data{{"area", "blender"}, {"reason", reason}};
                if (!message.isEmpty())
                    data.insert(QStringLiteral("message"), message);
                m_ws->broadcastEvent(QStringLiteral("error.card"), data);
            });
    connect(m_cloud.get(), &Cloud3dService::assetCreated, this,
            [this](const QString& glbPath) {
                const QString relative = m_library->relativeFor(glbPath);
                if (relative.isEmpty())
                    return;
                const QString area = QStringLiteral("blender");
                const QString name = QFileInfo(glbPath).completeBaseName();
                const QString dir = QFileInfo(relative).path();

                // 卡片参数：服务商 + 模型/版本（Tripo 的模型位是 version）
                const QJsonObject cloud =
                    m_settings->data().value(QStringLiteral("cloud3d")).toObject();
                const QString provider =
                    cloud.value(QStringLiteral("provider")).toString();
                const QJsonObject providerCfg =
                    cloud.value(provider).toObject();
                QString model = providerCfg.value(QStringLiteral("model")).toString();
                if (model.isEmpty())
                    model = providerCfg.value(QStringLiteral("version")).toString();

                AssetCardInput input;
                input.name = name;
                input.source = QStringLiteral("cloud3d");
                input.provider = provider;
                input.model = model;
                input.prompt = m_assetPrompts.take(area);
                input.paths = QJsonObject{{"glb", relative}};
                input.dir = dir == QLatin1String(".") ? QString() : dir;
                input.createdAt =
                    QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                const QJsonObject card = makeAssetCard(input);
                const qint64 cardId = m_sessions->appendMessage(
                    area, QStringLiteral("asset"),
                    QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact)),
                    QJsonObject{{"state", "done"}});

                m_ws->broadcastEvent(
                    QStringLiteral("asset.created"),
                    QJsonObject{{"area", area},
                                {"name", name},
                                {"path", relative},
                                {"paths", QJsonObject{{"glb", relative}}},
                                {"id", cardId},
                                {"card", card}});
                m_ws->broadcastEvent(
                    QStringLiteral("fileTree.changed"),
                    QJsonObject{{"area", area},
                                {"reason", "incremental"},
                                {"paths", QJsonArray{dir}}});
            });
    connect(m_summarizer, &SummarizerAgent::failed, this,
            [this](const QString& area, const QString& reason) {
                // 重新生成失败也要点名是哪张卡：前端靠它停转圈、恢复按钮
                if (const qint64 regenId = m_regenDraft.take(area); regenId > 0) {
                    m_ws->broadcastEvent(
                        QStringLiteral("draft.failed"),
                        QJsonObject{{"area", area}, {"id", regenId}, {"reason", reason}});
                }
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area}, {"reason", reason}});
            });

    // 编排器：计划卡与分组执行。draft.send → plan()，plan.confirm → 选目录 → execute()
    m_orch = new AgentOrchestrator(m_llm.get(), m_blender->client(), this);
    m_orch->setBlenderGate(&m_blenderGate); // 仅 blender 编排器持有租约
    connect(m_orch, &AgentOrchestrator::toolStep, this,
            [this](const QString& area, const QString& tool, const QString& status) {
                m_ws->broadcastEvent(QStringLiteral("tool.step"),
                                     QJsonObject{{"area", area},
                                                 {"tool", tool},
                                                 {"status", status}});
            });
    connect(m_orch, &AgentOrchestrator::delta, this,
            [this](const QString& area, const QString& text) {
                m_ws->broadcastEvent(QStringLiteral("chat.delta"),
                                     QJsonObject{{"area", area}, {"text", text}});
            });
    connect(m_orch, &AgentOrchestrator::turnFinished, this,
            [this](const QString& area, const QString& text) {
                if (text.trimmed().isEmpty())
                    return;
                // 中间轮次也要落库：刷新/崩溃后能看到每步说明，而不是只剩最后一句
                m_sessions->appendMessage(area, QStringLiteral("assistant"), text);
                m_ws->broadcastEvent(QStringLiteral("chat.turn"),
                                     QJsonObject{{"area", area}, {"content", text}});
            });
    connect(m_orch, &AgentOrchestrator::finalText, this,
            [this](const QString& area, const QString& text) {
                m_sessions->appendMessage(area, QStringLiteral("assistant"), text);
                m_ws->broadcastEvent(QStringLiteral("chat.message"),
                                     QJsonObject{{"area", area},
                                                 {"role", "assistant"},
                                                 {"content", text}});
                // 导出回合正常结束（即使模型没产出新 glb）也要解锁预览
                m_ws->broadcastEvent(QStringLiteral("export.finished"),
                                     QJsonObject{{"area", area}, {"completed", true}});
            });
    connect(m_orch, &AgentOrchestrator::failed, this,
            [this](const QString& area, const QString& reason) {
                // 步数耗尽时前端给「继续工作」：带上用量，让用户知道还差多少
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area},
                                                 {"reason", reason},
                                                 {"resumable", m_orch->canResume(area)},
                                                 {"steps", m_orch->stepCount()},
                                                 {"maxSteps", m_orch->stepBudget()}});
            });
    connect(m_orch, &AgentOrchestrator::groupProgress, this,
            [this](const QString& area, int index, int count, const QString& title) {
                m_ws->broadcastEvent(QStringLiteral("task.progress"),
                                     QJsonObject{{"area", area},
                                                 {"groupIndex", index},
                                                 {"groupCount", count},
                                                 {"title", title}});
            });
    connect(m_orch, &AgentOrchestrator::imageReceived, this,
            [this](const QString& area, const QString& taskId, const QString& dataUrl,
                   const QString& caption) {
                m_ws->broadcastEvent(QStringLiteral("chat.image"),
                                     QJsonObject{{"area", area},
                                                 {"taskId", taskId},
                                                 {"dataUrl", dataUrl},
                                                 {"caption", caption}});
            });
    connect(m_orch, &AgentOrchestrator::planReady, this,
            [this](const QString& area, const QString& taskId, const QJsonObject& plan) {
                if (m_planCancelled.remove(area))
                    return; // 已取消的规划不再弹卡
                // 计划卡也是一行：刷新后靠 content + meta.state 重建
                const QJsonObject payload{
                    {QStringLiteral("taskId"), taskId},
                    {QStringLiteral("rules"), plan.value(QStringLiteral("rules"))},
                    {QStringLiteral("groups"), plan.value(QStringLiteral("groups"))},
                    {QStringLiteral("totalSteps"), plan.value(QStringLiteral("totalSteps"))}};
                const qint64 planId = m_sessions->appendMessage(
                    area, QStringLiteral("plan"),
                    QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)),
                    QJsonObject{{QStringLiteral("state"), QStringLiteral("pending")},
                                {QStringLiteral("taskId"), taskId}});
                // id 也放进暂存：历史遗留的旧 pending 卡点确认时能和当前计划对上
                QJsonObject pending = plan;
                pending.insert(QStringLiteral("id"), planId);
                m_pendingPlans.insert(area, pending);
                m_ws->broadcastEvent(
                    QStringLiteral("plan.created"),
                    QJsonObject{{QStringLiteral("area"), area},
                                {QStringLiteral("id"), planId},
                                {QStringLiteral("taskId"), taskId},
                                {QStringLiteral("rules"), plan.value(QStringLiteral("rules"))},
                                {QStringLiteral("groups"), plan.value(QStringLiteral("groups"))},
                                {QStringLiteral("totalSteps"), plan.value(QStringLiteral("totalSteps"))}});
            });
    connect(m_orch, &AgentOrchestrator::exportStarted, this,
            [this](const QString& area, const QString& folder) {
                // 预览面板先挂“导出中”：glb 还在写，不能直接加载
                m_ws->broadcastEvent(QStringLiteral("export.started"),
                                     QJsonObject{{"area", area}, {"folder", folder}});
            });
    connect(m_orch, &AgentOrchestrator::artifactsReady, this,
            [this](const QString& area, const QStringList& absoluteGlbs) {
                if (!m_library || area != QLatin1String("blender"))
                    return;
                // 每个 GLB 配同基名的 fbx/blend 兄弟文件（不存在的后缀不收录），供前端自动预览
                const QString prompt = m_assetPrompts.take(area);
                const QString createdAt =
                    QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                QJsonArray changedDirs;
                for (const QString& abs : absoluteGlbs) {
                    const QString relative = m_library->relativeFor(abs);
                    if (relative.isEmpty())
                        continue; // 导出到了资产库外（用户手工改过路径）就不管
                    const QString dir = QFileInfo(relative).path();
                    const QString base = QFileInfo(relative).completeBaseName();
                    QJsonObject paths{{"glb", relative}};
                    for (const QString& suffix : {QStringLiteral("fbx"), QStringLiteral("blend")}) {
                        const QString sibling =
                            (dir == QLatin1String(".") ? QString() : dir + '/')
                            + base + '.' + suffix;
                        if (QFileInfo::exists(m_library->absoluteFor(sibling)))
                            paths.insert(suffix, sibling);
                    }
                    AssetCardInput input;
                    input.name = base;
                    input.source = QStringLiteral("lowpoly");
                    input.prompt = prompt;
                    input.paths = paths;
                    input.dir = dir == QLatin1String(".") ? QString() : dir;
                    input.createdAt = createdAt;
                    const QJsonObject card = makeAssetCard(input);
                    const qint64 cardId = m_sessions->appendMessage(
                        area, QStringLiteral("asset"),
                        QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact)),
                        QJsonObject{{"state", "done"}});
                    m_ws->broadcastEvent(QStringLiteral("asset.created"),
                                         QJsonObject{{"area", area},
                                                     {"name", base},
                                                     {"path", relative},
                                                     {"paths", paths},
                                                     {"id", cardId},
                                                     {"card", card}});
                    changedDirs.append(dir);
                }
                if (!changedDirs.isEmpty())
                    m_ws->broadcastEvent(QStringLiteral("fileTree.changed"),
                                         QJsonObject{{"area", area},
                                                     {"reason", "incremental"},
                                                     {"paths", changedDirs}});
            });

    m_router->registerHandler(QStringLiteral("settings.get"), [this](const QJsonObject&) {
        return QJsonValue(m_settings->data());
    });
    m_router->registerHandler(QStringLiteral("settings.save"),
                              [this](const QJsonObject& params) {
                                  m_settings->setData(params);
                                  if (!m_settings->save())
                                      throw std::runtime_error("settings save failed");
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    // 界面偏好存 ui.ini：页面 localStorage 在 app:// 下重启不保留
    m_router->registerHandler(QStringLiteral("uiState.get"), [this](const QJsonObject&) {
        return QJsonValue(QJsonObject{{"blenderMode", m_uiState->blenderMode()},
                                      {"chatMode", m_uiState->chatMode()},
                                      {"shape", m_uiState->shape()}});
    });
    m_router->registerHandler(QStringLiteral("uiState.set"), [this](const QJsonObject& params) {
        if (params.contains(QStringLiteral("blenderMode"))) {
            const QString mode = params.value(QStringLiteral("blenderMode")).toString();
            if (mode != QLatin1String("lowpoly") && mode != QLatin1String("cloud3d"))
                throw std::runtime_error("invalid blenderMode");
            m_uiState->setBlenderMode(mode);
        }
        if (params.contains(QStringLiteral("chatMode"))) {
            const QString mode = params.value(QStringLiteral("chatMode")).toString();
            if (mode != QLatin1String("ask") && mode != QLatin1String("exec"))
                throw std::runtime_error("invalid chatMode");
            m_uiState->setChatMode(mode);
        }
        if (params.contains(QStringLiteral("shape"))) {
            const QString shape = params.value(QStringLiteral("shape")).toString();
            // 合法键表只在 ShapePolicy 一份：往返不变就说明是三个键之一
            if (shapeKey(shapeFromString(shape)) != shape)
                throw std::runtime_error("invalid shape");
            m_uiState->setShape(shape);
        }
        return QJsonValue(QJsonObject{{"ok", true}});
    });
    m_router->registerHandler(QStringLiteral("session.load"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  const int limit =
                                      params.value(QStringLiteral("limit")).toInt(); // 0 → 默认 20
                                  const qint64 beforeId = static_cast<qint64>(
                                      params.value(QStringLiteral("beforeId")).toDouble());
                                  const MessagePage page =
                                      m_sessions->messagePage(area, limit, beforeId);
                                  return QJsonValue(QJsonObject{
                                      {"messages", page.messages},
                                      {"hasMore", page.hasMore},
                                      {"nextCursor", page.nextCursor}});
                              });
    m_router->registerHandler(QStringLiteral("chat.send"),
                              [this](const QJsonObject& params) {
                                  return handleChatSend(params);
                              });
    m_router->registerHandler(QStringLiteral("draft.save"),
                              [this](const QJsonObject& params) {
                                  const qint64 id = params.value(QStringLiteral("id")).toInteger();
                                  const QString content =
                                      params.value(QStringLiteral("content")).toString();
                                  if (id <= 0)
                                      return QJsonValue(QJsonObject{{"ok", false}});
                                  // 编辑失焦即存：刷新后看到的是改过的版本
                                  return QJsonValue(
                                      QJsonObject{{"ok", m_sessions->updateMessageContent(id,
                                                                                          content)}});
                              });
    m_router->registerHandler(QStringLiteral("draft.send"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QString prompt = params.value(QStringLiteral("prompt")).toString().trimmed();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty() || prompt.isEmpty())
            return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "invalid-draft"}});

        const QString area = row.value(QStringLiteral("area")).toString();
        const QJsonObject meta = row.value(QStringLiteral("meta")).toObject();
        const QString asset = meta.value(QStringLiteral("asset")).toString();
        const bool cloud =
            meta.value(QStringLiteral("mode")).toString() == QLatin1String("cloud3d");

        // 草稿阶段不拦 Blender（写方案只需 LLM）；点执行才检查，没开就弹提示且不占门禁。
        // 云模式不经过 Blender，跳过这项检查
        if (!cloud) {
            const PreflightResult pre = m_preflight->checkLowpoly();
            if (!pre.ok) {
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area}, {"reason", pre.reason}});
                return QJsonValue(QJsonObject{{"accepted", false}, {"reason", pre.reason}});
            }
        }

        // 先确认源文件在，再转正：否则草稿卡消失了但执行失败，用户无法回头改
        if (!asset.isEmpty()) {
            QFileInfo src(m_library->absoluteFor(asset));
            if (src.suffix().toLower() != QLatin1String("blend"))
                src = QFileInfo(src.dir().filePath(src.completeBaseName()
                                                   + QStringLiteral(".blend")));
            if (!src.exists())
                return QJsonValue(QJsonObject{{"accepted", false},
                                              {"error", "no-blend-source"}});
        }
        // 草稿转正：改角色而不是新增 user 消息，否则历史里同一段文本出现两次
        m_sessions->updateMessageContent(id, prompt);
        m_sessions->setMessageRole(id, QStringLiteral("user"));
        m_ws->broadcastEvent(QStringLiteral("draft.done"),
                             QJsonObject{{"area", area}, {"id", id}});
        m_ws->broadcastEvent(QStringLiteral("chat.message"),
                             QJsonObject{{"area", area}, {"role", "user"},
                                         {"content", prompt}});

        // 云模式直接选目录，不经过低模的 plan 阶段
        if (cloud) {
            m_pendingPicks.insert(area,
                                  PendingPick{area, QStringLiteral("cloud3d"), prompt, {}});
            openFolderPick(area);
            return QJsonValue(QJsonObject{{"accepted", true}});
        }
        if (!asset.isEmpty())
            return startModify(area, asset, prompt);
        // plan.confirm 要用用户最后编辑过的提示词去执行，不是 Summarizer 的初稿
        m_planPrompts.insert(area, prompt);
        // 新指令作废上一次「取消规划」标记，否则 planReady 会把这次的新计划也丢掉
        m_planCancelled.remove(area);
        m_orch->setModelConfig(primaryAgentConfig());
        // 本次指令已由草稿行转正落库，plan() 会把它拼在末尾：历史里去掉同一条避免重复
        m_orch->setConversationHistory(historyForPrompt(area, prompt, 20));
        m_orch->plan(QStringLiteral("plan-%1").arg(++m_seq), nullptr, prompt, area);
        return QJsonValue(QJsonObject{{"accepted", true}});
                              });
    m_router->registerHandler(QStringLiteral("draft.cancel"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty())
            return QJsonValue(QJsonObject{{"ok", false}});
        QJsonObject meta = row.value(QStringLiteral("meta")).toObject();
        meta.insert(QStringLiteral("state"), QStringLiteral("cancelled"));
        m_sessions->updateMessageMeta(id, meta);
        m_ws->broadcastEvent(
            QStringLiteral("draft.done"),
            QJsonObject{{"area", row.value(QStringLiteral("area")).toString()},
                        {"id", id}, {"cancelled", true}});
        return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("draft.regenerate"),
                              [this](const QJsonObject& params) {
        const qint64 id = params.value(QStringLiteral("id")).toInteger();
        const QJsonObject row = id > 0 ? m_sessions->messageById(id) : QJsonObject{};
        if (row.isEmpty())
            return QJsonValue(QJsonObject{{"accepted", false}});

        const QString area = row.value(QStringLiteral("area")).toString();
        const QJsonObject meta = row.value(QStringLiteral("meta")).toObject();
        const QString asset = meta.value(QStringLiteral("asset")).toString();
        const QString source = meta.value(QStringLiteral("source")).toString();
        const QString draftMode = meta.value(QStringLiteral("mode")).toString();
        const QString mode = draftMode.isEmpty()
            ? (asset.isEmpty() ? QStringLiteral("lowpoly") : QStringLiteral("modify"))
            : draftMode;
        // 讨论产出的正文本身就是讨论结论；SummarizerAgent 产出的不能再当讨论喂回去
        const QString discussion =
            meta.value(QStringLiteral("origin")).toString() == QLatin1String("discussion")
                ? row.value(QStringLiteral("content")).toString()
                : QString();

        m_regenDraft.insert(area, id);
        m_summarizer->generate({area, source.isEmpty() ? discussion : source, mode,
                                QFileInfo(asset).completeBaseName(), discussion});
        return QJsonValue(QJsonObject{{"accepted", true}});
                              });
    m_router->registerHandler(QStringLiteral("plan.confirm"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  // 先核对 id 是不是当前这张；历史遗留卡只置灰自己
                                  const qint64 requestedId =
                                      params.value(QStringLiteral("id")).toInteger();
                                  const qint64 pendingId = m_pendingPlans.value(area)
                                                               .value(QStringLiteral("id"))
                                                               .toInteger();
                                  if (requestedId > 0 && requestedId != pendingId) {
                                      // pendingId==0：应用重启后内存里没计划；不相等：点的是旧卡。
                                      // 只改仍为 pending 的卡，避免把已 started 的卡又改回 cancelled
                                      const QJsonObject row =
                                          m_sessions->messageById(requestedId);
                                      QJsonObject meta = row.value(QStringLiteral("meta"))
                                                             .toObject();
                                      if (row.value(QStringLiteral("role")).toString()
                                              == QLatin1String("plan")
                                          && meta.value(QStringLiteral("state")).toString()
                                                 == QLatin1String("pending")) {
                                          meta.insert(QStringLiteral("state"),
                                                      QStringLiteral("cancelled"));
                                          m_sessions->updateMessageMeta(requestedId, meta);
                                      }
                                      return QJsonValue(QJsonObject{{"ok", false},
                                                                    {"error", "plan-not-found"}});
                                  }
                                  const QJsonObject json = m_pendingPlans.value(area);
                                  if (json.isEmpty())
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "plan-not-found"}});
                                  if (m_blenderGate.isBusy()) {
                                      // 忙时保留计划卡：用户等 Blender 空闲后可再点开始
                                      m_ws->broadcastEvent(
                                          QStringLiteral("error.card"),
                                          QJsonObject{{"area", area}, {"reason", "blender-busy"}});
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "blender-busy"}});
                                  }
                                  m_pendingPlans.remove(area);
                                  // 只有真正开始选目录才置 started；blender-busy 保持 pending
                                  const QJsonObject row = m_sessions->messageById(requestedId);
                                  if (requestedId > 0
                                      && row.value(QStringLiteral("role")).toString()
                                             == QLatin1String("plan")) {
                                      QJsonObject meta = row.value(QStringLiteral("meta"))
                                                             .toObject();
                                      meta.insert(QStringLiteral("state"),
                                                  QStringLiteral("started"));
                                      m_sessions->updateMessageMeta(requestedId, meta);
                                  }
                                  m_pendingPicks.insert(
                                      area, PendingPick{area, QStringLiteral("lowpoly"),
                                                        m_planPrompts.value(area),
                                                        buildPlanFromJson(json)});
                                  openFolderPick(area);
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("plan.cancel"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  // 历史遗留卡只置灰自己，不取消当前计划
                                  const qint64 requestedId =
                                      params.value(QStringLiteral("id")).toInteger();
                                  const qint64 pendingId = m_pendingPlans.value(area)
                                                               .value(QStringLiteral("id"))
                                                               .toInteger();
                                  if (requestedId > 0 && requestedId != pendingId) {
                                      const QJsonObject row =
                                          m_sessions->messageById(requestedId);
                                      QJsonObject meta = row.value(QStringLiteral("meta"))
                                                             .toObject();
                                      if (row.value(QStringLiteral("role")).toString()
                                              == QLatin1String("plan")
                                          && meta.value(QStringLiteral("state")).toString()
                                                 == QLatin1String("pending")) {
                                          meta.insert(QStringLiteral("state"),
                                                      QStringLiteral("cancelled"));
                                          m_sessions->updateMessageMeta(requestedId, meta);
                                      }
                                      return QJsonValue(QJsonObject{{"ok", true}});
                                  }
                                  m_planCancelled.insert(area);
                                  m_pendingPlans.remove(area);
                                  m_pendingPicks.remove(area);
                                  // 正常取消：只把 pending 的卡置灰
                                  if (requestedId > 0) {
                                      const QJsonObject row =
                                          m_sessions->messageById(requestedId);
                                      QJsonObject meta = row.value(QStringLiteral("meta"))
                                                             .toObject();
                                      if (row.value(QStringLiteral("role")).toString()
                                              == QLatin1String("plan")
                                          && meta.value(QStringLiteral("state")).toString()
                                                 == QLatin1String("pending")) {
                                          meta.insert(QStringLiteral("state"),
                                                      QStringLiteral("cancelled"));
                                          m_sessions->updateMessageMeta(requestedId, meta);
                                      }
                                  }
                                  m_ws->broadcastEvent(
                                      QStringLiteral("error.card"),
                                      QJsonObject{{"area", area}, {"reason", "user-cancelled"}});
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("folderPick.submit"),
                              [this](const QJsonObject& params) {
                                  const QString requestId =
                                      params.value(QStringLiteral("requestId")).toString();
                                  const QString area = m_pickRequests.value(requestId);
                                  if (area.isEmpty() || !m_pendingPicks.contains(area))
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "request-not-found"}});
                                  const QString relative =
                                      params.value(QStringLiteral("relativePath")).toString();
                                  if (!m_library->isValidTarget(relative)
                                      || !QDir(m_library->absoluteFor(relative)).exists())
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "invalid-path"}});
                                  const PendingPick pick = m_pendingPicks.value(area);
                                  // 云任务不占 Blender 门禁，选完目录直接提交
                                  if (pick.mode == QLatin1String("cloud3d")) {
                                      m_pickRequests.remove(requestId);
                                      m_pendingPicks.remove(area);
                                      if (!m_cloud)
                                          return QJsonValue(QJsonObject{
                                              {"ok", false}, {"error", "cloud3d-unavailable"}});
                                      m_assetPrompts.insert(area, pick.prompt);
                                      m_cloud->start(pick.prompt, relative);
                                      return QJsonValue(QJsonObject{{"ok", true}});
                                  }
                                  if (m_blenderGate.isBusy()) {
                                      // 保留 requestId 与 pendingPick：弹窗不关，空闲后可再确认
                                      m_ws->broadcastEvent(
                                          QStringLiteral("error.card"),
                                          QJsonObject{{"area", area}, {"reason", "blender-busy"}});
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "blender-busy"}});
                                  }
                                  m_pickRequests.remove(requestId);
                                  m_pendingPicks.remove(area);
                                  const QString targetFolder = m_library->absoluteFor(relative);
                                  m_assetPrompts.insert(area, pick.prompt);
                                  const QString taskId = QStringLiteral("build-%1").arg(++m_seq);
                                  m_tasks->enqueue(Task{
                                      taskId, area, QString(),
                                      [this, pick, targetFolder](const QString& tid,
                                                                 TaskManager* tm) {
                                          m_orch->setModelConfig(primaryAgentConfig());
                                          m_orch->setConversationHistory(
                                              historyForPrompt(pick.area, pick.prompt, 20));
                                          m_orch->execute(
                                              tid, tm, ToolBridge::mcpToolsToOpenAi(m_blenderTools),
                                              pick.plan, pick.prompt, targetFolder, pick.area);
                                      }});
                                  return QJsonValue(
                                      QJsonObject{{"ok", true}, {"taskId", taskId}});
                              });
    m_router->registerHandler(QStringLiteral("folderPick.cancel"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      m_pickRequests.take(params.value("requestId").toString());
                                  if (area.isEmpty()) // 旧弹窗（requestId 已被取代）不报错卡
                                      return QJsonValue(QJsonObject{{"ok", true}});
                                  m_pendingPicks.remove(area);
                                  m_ws->broadcastEvent(
                                      QStringLiteral("error.card"),
                                      QJsonObject{{"area", area}, {"reason", "user-cancelled"}});
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("folderPick.mkdir"),
                              [this](const QJsonObject& params) {
                                  const QString parent = params.value("path").toString();
                                  const QString error = m_library->makeFolder(
                                      parent, params.value("name").toString());
                                  if (!error.isEmpty())
                                      return QJsonValue(
                                          QJsonObject{{"ok", false}, {"error", error}});
                                  // 左栏文件树立刻能看到新目录，不用等导出
                                  m_ws->broadcastEvent(
                                      QStringLiteral("fileTree.changed"),
                                      QJsonObject{{"area", "blender"},
                                                  {"reason", "incremental"},
                                                  {"paths", QJsonArray{parent}}});
                                  return QJsonValue(QJsonObject{{"ok", true},
                                                                {"tree", m_library->folderTree()}});
                              });
    m_router->registerHandler(QStringLiteral("assets.list"), [this](const QJsonObject&) {
        QJsonArray items;
        // 先放目录（含空目录）：左栏树不能只靠模型文件反推目录，否则新建的空文件夹看不见
        for (const QString& relative : m_library->scanFolders()) {
            const QFileInfo info(m_library->absoluteFor(relative));
            items.append(QJsonObject{{"name", info.fileName()},
                                     {"relativePath", relative},
                                     {"kind", "dir"},
                                     {"size", 0},
                                     {"modified", info.lastModified().toString(Qt::ISODate)}});
        }
        const QHash<QString, QString> cloudAssets = m_library->cloudAssets();
        for (const QString& relative : m_library->scanAssets()) {
            const QFileInfo info(m_library->absoluteFor(relative));
            QJsonObject item{{"name", info.completeBaseName()},
                             {"relativePath", relative},
                             {"kind", info.suffix().toLower()},
                             {"size", static_cast<double>(info.size())},
                             {"modified", info.lastModified().toString(Qt::ISODate)}};
            // 云生成的文件带来源：文件树据此标「云 · glb」
            if (const auto it = cloudAssets.constFind(relative); it != cloudAssets.cend()) {
                item.insert(QStringLiteral("source"), QStringLiteral("cloud"));
                item.insert(QStringLiteral("provider"), it.value());
            }
            items.append(item);
        }
        return QJsonValue(items);
    });
    m_router->registerHandler(
        QStringLiteral("file.read"), [this](const QJsonObject& params) {
            const QString area = params.value("area").toString(QStringLiteral("blender"));
            if (area != QLatin1String("blender")) // unity 工程内文件由 P6 接管
                return QJsonValue(
                    QJsonObject{{"ok", false}, {"error", "area-not-supported"}});
            const QString path = params.value("path").toString();
            if (!m_library->isValidTarget(path))
                return QJsonValue(QJsonObject{{"ok", false}, {"error", "invalid-path"}});
            const QString abs = m_library->absoluteFor(path);
            const QFileInfo info(abs);
            if (!info.exists() || info.isDir())
                return QJsonValue(QJsonObject{{"ok", false}, {"error", "not-found"}});
            const QString suffix = info.suffix().toLower();
            if (QStringList{QStringLiteral("png"), QStringLiteral("jpg"),
                            QStringLiteral("jpeg"), QStringLiteral("webp")}
                    .contains(suffix)) {
                QFile file(abs);
                if (!file.open(QIODevice::ReadOnly))
                    return QJsonValue(QJsonObject{{"ok", false}, {"error", "io-error"}});
                QString mime = QStringLiteral("image/jpeg");
                if (suffix == QLatin1String("png"))
                    mime = QStringLiteral("image/png");
                else if (suffix == QLatin1String("webp"))
                    mime = QStringLiteral("image/webp");
                return QJsonValue(
                    QJsonObject{{"ok", true},
                                {"kind", "image"},
                                {"dataUrl", QStringLiteral("data:%1;base64,").arg(mime)
                                                + QString::fromLatin1(file.readAll().toBase64())}});
            }
            if (suffix == QLatin1String("glb")) {
                // 模型三种格式都回 blendPath：修改入口据此判断有没有可编辑源文件
                const QFileInfo blend(info.dir().filePath(info.completeBaseName()
                                                          + QStringLiteral(".blend")));
                return QJsonValue(QJsonObject{
                    {"ok", true},
                    {"kind", "model"},
                    {"dataUrl", QUrl::fromLocalFile(abs).toString()},
                    // 前端加载前轮询这两个字段，确认导出已经写完再交给 three.js
                    {"size", static_cast<double>(info.size())},
                    {"modified", info.lastModified().toString(Qt::ISODate)},
                    {"blendPath", blend.exists()
                                      ? m_library->relativeFor(blend.absoluteFilePath())
                                      : QString()}});
            }
            // fbx / blend 不读内容（可能几十 MB），只回文件信息卡
            if (suffix == QLatin1String("fbx") || suffix == QLatin1String("blend")) {
                const QFileInfo blend =
                    suffix == QLatin1String("blend")
                        ? info
                        : QFileInfo(info.dir().filePath(info.completeBaseName()
                                                        + QStringLiteral(".blend")));
                return QJsonValue(QJsonObject{
                    {"ok", true},
                    {"kind", "info"},
                    {"size", static_cast<double>(info.size())},
                    {"modified", info.lastModified().toString(Qt::ISODate)},
                    {"blendPath", blend.exists()
                                      ? m_library->relativeFor(blend.absoluteFilePath())
                                      : QString()}});
            }
            QFile file(abs);
            if (!file.open(QIODevice::ReadOnly))
                return QJsonValue(QJsonObject{{"ok", false}, {"error", "io-error"}});
            if (file.size() > 2 * 1024 * 1024)
                return QJsonValue(QJsonObject{{"ok", false}, {"error", "too-large"}});
            const QString language = suffix == QLatin1String("cs") ? QStringLiteral("csharp")
                                     : suffix == QLatin1String("lua") ? QStringLiteral("lua")
                                     : suffix == QLatin1String("json") ? QStringLiteral("json")
                                                                       : QStringLiteral("typescript");
            return QJsonValue(QJsonObject{{"ok", true},
                                          {"kind", "text"},
                                          {"content", QString::fromUtf8(file.readAll())},
                                          {"language", language}});
        });
    m_router->registerHandler(QStringLiteral("fs.operate"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value("area").toString(QStringLiteral("blender"));
                                  if (area != QLatin1String("blender")) // unity 区由 P6 接管
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "area-not-supported"}});
                                  const QString op = params.value("op").toString();
                                  const QString path = params.value("path").toString();
                                  QString error;
                                  if (op == QLatin1String("mkdir"))
                                      error = m_library->makeFolder(
                                          path, params.value("name").toString());
                                  else if (op == QLatin1String("rename"))
                                      error = m_library->renameEntry(
                                          path, params.value("name").toString());
                                  else if (op == QLatin1String("delete"))
                                      error = m_library->deleteEntry(path);
                                  else if (op == QLatin1String("reveal")) {
                                      if (!m_library->revealEntry(path))
                                          error = QStringLiteral("not-found");
                                  } else if (op != QLatin1String("refresh"))
                                      error = QStringLiteral("unknown-op");
                                  if (!error.isEmpty())
                                      return QJsonValue(
                                          QJsonObject{{"ok", false}, {"error", error}});
                                  if (op != QLatin1String("reveal")) // 打开资源管理器不改树
                                      m_ws->broadcastEvent(
                                          QStringLiteral("fileTree.changed"),
                                          QJsonObject{{"area", "blender"},
                                                      {"reason", op == QLatin1String("refresh")
                                                                     ? "full"
                                                                     : "incremental"},
                                                      {"paths", op == QLatin1String("refresh")
                                                                    ? QJsonArray{}
                                                                    : QJsonArray{path}}});
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("chat.retry"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  const QJsonObject last = m_lastSend.value(area);
                                  if (last.isEmpty())
                                      return QJsonValue(QJsonObject{
                                          {"accepted", false}, {"error", "no-task-to-retry"}});
                                  // 云任务不在任务队列：续轮询/重新下载，不重复提交
                                  if (last.value(QStringLiteral("mode")).toString()
                                          == QLatin1String("cloud3d")
                                      && m_cloud && m_cloud->canRetry())
                                      return QJsonValue(
                                          QJsonObject{{"accepted", m_cloud->retry()}});
                                  // 不重复落库同一句 user 消息
                                  return handleChatSend(last, /*appendUser=*/false);
                              });
    m_router->registerHandler(QStringLiteral("chat.cancel"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  // 先打标记再断网络：abort 触发的 onError 必须能查到 cancelled
                                  m_tasks->cancelArea(area);
                                  m_llm->abortTag(area);
                                  if (m_orch->isPlanning(area)) { // 规划不在 TaskManager 队列，需单独丢结果
                                      m_planCancelled.insert(area);
                                      m_pendingPlans.remove(area);
                                  }
                                  // 云生成不在任务队列：停轮询并丢弃迟到回调
                                  if (area == QLatin1String("blender") && m_cloud
                                      && m_cloud->isRunning())
                                      m_cloud->cancel();
                                  // 导出回合被停止：预览面板解锁，别一直挂着“导出中”
                                  m_ws->broadcastEvent(QStringLiteral("export.finished"),
                                                       QJsonObject{{"area", area},
                                                                   {"cancelled", true}});
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("chat.continue"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  if (!m_orch->canResume(area))
                                      return QJsonValue(QJsonObject{{"ok", false},
                                                                    {"error", "nothing-to-resume"}});
                                  if (m_blenderGate.isBusy()) // 稍后再点，现场还在
                                      return QJsonValue(QJsonObject{{"ok", false},
                                                                    {"error", "blender-busy"}});
                                  const QString taskId = QStringLiteral("resume-%1").arg(++m_seq);
                                  m_tasks->enqueue(Task{
                                      taskId, area, QString(),
                                      [this](const QString& tid, TaskManager* tm) {
                                          m_orch->setModelConfig(primaryAgentConfig());
                                          m_orch->resume(tid, tm);
                                      }});
                                  return QJsonValue(
                                      QJsonObject{{"ok", true}, {"taskId", taskId}});
                              });
    m_router->registerHandler(QStringLiteral("chat.clear"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  m_tasks->cancelArea(area); // 正在跑的任务放弃回写
                                  m_llm->abortTag(area);
                                  m_sessions->clearMessages(area);
                                  m_lastSend.remove(area);
                                  return QJsonValue(QJsonObject{{"ok", true}});
                              });
    m_router->registerHandler(QStringLiteral("bridge.restart"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  const RestartFn fn = m_bridgeRestarts.value(area);
                                  if (!fn)
                                      return QJsonValue(QJsonObject{
                                          {"ok", false}, {"error", "bridge-not-registered"}});
                                  return QJsonValue(QJsonObject{{"ok", fn()}});
                              });

    if (!m_ws->listen())
        return false;

    if (m_options.headless) {
        std::fprintf(stderr, "READY ws=ws://127.0.0.1:%u/?token=%s\n",
                     static_cast<unsigned>(m_ws->port()),
                     m_ws->token().toUtf8().constData());
        std::fflush(stderr);
    }
    m_blender->start(); // 异步启动：握手完成后通过 conn.status 点亮顶栏
    return true;
}

void CoreApp::registerBridge(const QString& area, RestartFn fn)
{
    m_bridgeRestarts.insert(area, std::move(fn));
}

void CoreApp::broadcastConnStatus(const QString& area, const QString& status,
                                  const QString& hint)
{
    m_status.insert(area, status);
    if (hint.isEmpty())
        m_hints.remove(area); // ready 时清掉旧的“第 N 次重连”提示
    else
        m_hints.insert(area, hint);

    broadcastStatusSnapshot();

    if (status == QLatin1String("ready")) {
        m_everReady.insert(area);
        m_crashNotified.remove(area); // 恢复后再次崩溃会重新出卡
    } else if (status == QLatin1String("off") && m_everReady.contains(area)
               && !m_crashNotified.contains(area)) {
        m_crashNotified.insert(area); // 一次断链只报一张卡（重试循环不重复弹）
        m_ws->broadcastEvent("error.card",
                             QJsonObject{{"area", area},
                                         {"reason", "bridge-crashed"},
                                         {"message", hint}});
    }
}

void CoreApp::broadcastStatusSnapshot() const
{
    QJsonObject hints;
    for (auto it = m_hints.constBegin(); it != m_hints.constEnd(); ++it)
        hints.insert(it.key(), it.value());

    m_ws->broadcastEvent(QStringLiteral("conn.status"),
                         QJsonObject{{"blender", m_status.value(QStringLiteral("blender"), "off")},
                                     {"unity", m_status.value(QStringLiteral("unity"), "off")},
                                     {"hints", hints}});
}

LlmConfig CoreApp::primaryAgentConfig() const
{
    const QJsonObject data = m_settings->data();
    const QJsonObject primary = data.value(QStringLiteral("agent"))
                                   .toObject()
                                   .value(QStringLiteral("primary"))
                                   .toObject();
    return {resolveAgentBaseUrl(primary, data), primary.value(QStringLiteral("apiKey")).toString(),
            primary.value(QStringLiteral("model")).toString()};
}

QJsonArray CoreApp::recentMessages(const QString& area, int limit) const
{
    QJsonArray history;
    for (const QJsonValue& value : m_sessions->messagePage(area, limit).messages) {
        const QJsonObject message = value.toObject();
        // 草稿卡 / 计划卡是 UI 行，不是对话：喂给 LLM 会得到未知 role
        const QString role = message.value(QStringLiteral("role")).toString();
        if (role != QLatin1String("user") && role != QLatin1String("assistant"))
            continue;
        history.append(QJsonObject{{"role", role},
                                   {"content", message.value(QStringLiteral("content"))}});
    }
    return history;
}

QJsonArray CoreApp::historyForPrompt(const QString& area, const QString& prompt, int limit) const
{
    QJsonArray history = recentMessages(area, limit + 1);
    if (!history.isEmpty()) {
        const QJsonObject last = history.last().toObject();
        if (last.value(QStringLiteral("role")).toString() == QLatin1String("user")
            && last.value(QStringLiteral("content")).toString() == prompt)
            history.removeLast();
    }
    return history;
}

QJsonValue CoreApp::handleChatSend(const QJsonObject& params, bool appendUser)
{
    const QString area = params.value(QStringLiteral("area")).toString();
    const QString text = params.value(QStringLiteral("text")).toString();
    const QString mode = params.value(QStringLiteral("mode")).toString(); // 空 = P2 通用流

    // 前置检查只留云 API：低模/修改在草稿阶段不需要 Blender，点执行时才检查
    if (mode == QLatin1String("cloud3d")) {
        const PreflightResult pre = m_preflight->checkCloud3d();
        if (!pre.ok) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", pre.reason}});
            return QJsonValue(QJsonObject{{"accepted", false}});
        }
    }

    m_lastSend.insert(area, params);

    // 讨论模式：只聊天，不检查桥接、不占门禁、不写草稿以外的任务链
    if (mode == QLatin1String("ask")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{QStringLiteral("area"), area}, {QStringLiteral("role"), "user"},
                            {QStringLiteral("content"), text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
        }

        const QString asset = params.value(QStringLiteral("asset")).toString();
        QFile tpl(QStringLiteral(":/prompts/ask.md"));
        QString system;
        if (tpl.open(QIODevice::ReadOnly | QIODevice::Text))
            system = QString::fromUtf8(tpl.readAll());
        // 修改入口是同基名 .blend；只看所选文件会把只有 glb 的云资产误判成可修改
        QFileInfo sourceFile;
        if (!asset.isEmpty()) {
            sourceFile = QFileInfo(m_library->absoluteFor(asset));
            if (sourceFile.suffix().toLower() != QLatin1String("blend"))
                sourceFile = QFileInfo(sourceFile.dir().filePath(
                    sourceFile.completeBaseName() + QStringLiteral(".blend")));
        }
        const bool hasSource = sourceFile.exists();
        system.replace(QStringLiteral("{{asset}}"),
                       asset.isEmpty() ? QStringLiteral("（未选中）") : asset);
        system.replace(
            QStringLiteral("{{asset_state}}"),
            asset.isEmpty()
                ? QStringLiteral("无")
                : (hasSource ? QStringLiteral("已选中的低模资产")
                             : QStringLiteral("无 .blend 源文件，只能重新生成")));
        // 资产清单：让模型能说「你库里有三个招牌」而不是瞎猜；glb/fbx/blend 同基名只列一次
        QString assetLines;
        QSet<QString> seenBases;
        for (const QString& rel : m_library->scanAssets()) {
            const QFileInfo fi(m_library->absoluteFor(rel));
            if (fi.suffix().toLower() != QLatin1String("glb"))
                continue;
            const QString base = fi.completeBaseName();
            if (seenBases.contains(base))
                continue;
            seenBases.insert(base);
            const bool hasBlend =
                QFileInfo(fi.dir().filePath(base + QStringLiteral(".blend"))).exists();
            assetLines += QStringLiteral("- %1%2\n").arg(
                rel, hasBlend ? QStringLiteral("（有 .blend，可修改）")
                              : QStringLiteral("（无 .blend，只能重新生成）"));
            if (seenBases.size() >= 30)
                break;
        }
        system.replace(QStringLiteral("{{assets}}"),
                       assetLines.isEmpty() ? QStringLiteral("（空）") : assetLines);

        QJsonArray messages{
            QJsonObject{{"role", "system"}, {"content", system}}};
        // recentMessages 只放行 user/assistant：草稿卡与计划卡行不能喂给 LLM
        for (const QJsonValue& value : recentMessages(area, 20))
            messages.append(value);

        const QString taskId = QStringLiteral("ask-%1").arg(++m_seq);
        m_tasks->enqueue(Task{
            taskId, area, QString(),
            [this, area, cfg, messages, asset, text](const QString& tid, TaskManager* tm) {
                // [[EXEC]] 之后整段都是草稿：流里一个字都不能发，等结束做卡片
                auto buffer = std::make_shared<QString>();
                auto filter = std::make_shared<AskStreamFilter>();
                LlmGateway::StreamHandlers handlers;
                handlers.onDelta = [this, area, buffer, filter](const QString& delta) {
                    buffer->append(delta);
                    const QString visible = filter->feed(delta);
                    if (!visible.isEmpty())
                        m_ws->broadcastEvent(QStringLiteral("chat.delta"),
                                             QJsonObject{{"area", area}, {"text", visible}});
                };
                handlers.onDone = [this, area, tid, tm, buffer, filter, asset, text]() {
                    if (tm->isCancelled(tid)) {
                        m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                               QStringLiteral("cancelled"), {});
                        tm->finish(tid);
                        return;
                    }
                    const QString tail = filter->finish();
                    if (!tail.isEmpty())
                        m_ws->broadcastEvent(QStringLiteral("chat.delta"),
                                             QJsonObject{{"area", area}, {"text", tail}});
                    const AskReply reply = parseAskReply(*buffer);
                    m_sessions->appendMessage(area, QStringLiteral("assistant"), reply.visible);
                    m_ws->broadcastEvent(
                        QStringLiteral("chat.message"),
                        QJsonObject{{"area", area}, {"role", "assistant"},
                                    {"content", reply.visible}});
                    // 讨论产出的草稿也是一行 draft：刷新后靠它重建草稿卡
                    if (!reply.executable.isEmpty()) {
                        const qint64 draftId = m_sessions->appendMessage(
                            area, QStringLiteral("draft"), reply.executable,
                            QJsonObject{{"state", "pending"}, {"origin", "discussion"},
                                        {"asset", asset}, {"source", text}});
                        m_ws->broadcastEvent(
                            QStringLiteral("draft.created"),
                            QJsonObject{{"area", area}, {"id", draftId},
                                        {"prompt", reply.executable},
                                        {"origin", "discussion"}, {"asset", asset}});
                    }
                    m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                           QStringLiteral("done"), {},
                                           QJsonObject{{"reply", reply.visible}});
                    tm->finish(tid);
                };
                handlers.onError = [this, area, tid, tm](const QString& error) {
                    if (tm->isCancelled(tid)) {
                        m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                               QStringLiteral("cancelled"), {});
                        tm->finish(tid);
                        return;
                    }
                    m_ws->broadcastEvent(QStringLiteral("error.card"),
                                         QJsonObject{{"area", area}, {"reason", error}});
                    tm->finish(tid);
                };
                m_llm->chatStream(cfg, messages, {}, handlers, area);
            }});
        return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
    }

    // 修改已有资产：执行模式也先出修改草稿，指令模糊时由修改模板收敛
    if (mode == QLatin1String("modify")) {
        const QString asset = params.value(QStringLiteral("asset")).toString();
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(
                QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
        }
        m_pendingDraftAsset.insert(area, asset);
        m_pendingDraftSource.insert(area, text);
        m_summarizer->generate(
            {area, text, QStringLiteral("modify"), QFileInfo(asset).completeBaseName(), {}});
        return QJsonValue(QJsonObject{{"accepted", true}});
    }

    // 低模模式：先落库用户需求，再交给总结 agent 改写草稿（规格 5.2）
    if (mode == QLatin1String("lowpoly")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(
                QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
        }
        m_pendingDraftAsset.insert(area, QString());
        m_pendingDraftSource.insert(area, text);
        m_summarizer->generate({area, text}); // 默认 mode=lowpoly
        return QJsonValue(QJsonObject{{"accepted", true}});
    }

    if (mode == QLatin1String("cloud3d")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        const LlmConfig cfg = primaryAgentConfig();
        if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
            return QJsonValue(
                QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
        }
        m_pendingDraftAsset.insert(area, QString());
        m_pendingDraftSource.insert(area, text);
        m_pendingDraftMode.insert(area, QStringLiteral("cloud3d"));
        m_summarizer->generate({area, text, QStringLiteral("cloud3d"), {}, {}});
        return QJsonValue(QJsonObject{{"accepted", true}});
    }

    // 通用流（无模式兼容路径）：user 消息在这里落库
    if (appendUser) {
        m_sessions->appendMessage(area, QStringLiteral("user"), text);
        m_ws->broadcastEvent(QStringLiteral("chat.message"),
                             QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
    }

    const LlmConfig cfg = primaryAgentConfig();
    if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
        m_ws->broadcastEvent(QStringLiteral("error.card"),
                             QJsonObject{{"area", area}, {"reason", "agent-not-configured"}});
        return QJsonValue(
            QJsonObject{{"accepted", false}, {"reason", "agent-not-configured"}});
    }

    // 规格 8.4：只带本区最近 20 条纯文本历史（含刚落库的本次 user 消息）
    const QJsonArray history = recentMessages(area, 20);

    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);
    m_tasks->enqueue(Task{
        taskId, area, QString(),
        [this, area, cfg, history, params](const QString& tid, TaskManager* tm) {
            // 每任务独立缓冲：两个区并行时不能共用一个 QString
            auto buffer = std::make_shared<QString>();
            LlmGateway::StreamHandlers handlers;
            handlers.onDelta = [this, area, buffer](const QString& delta) {
                buffer->append(delta);
                m_ws->broadcastEvent(QStringLiteral("chat.delta"),
                                     QJsonObject{{"area", area}, {"text", delta}});
            };
            handlers.onDone = [this, area, tid, tm, buffer, params]() {
                // 取消/清空后迟到的 [DONE]：只记终态，不写库、不广播（否则消息"复活"）
                if (tm->isCancelled(tid)) {
                    m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                           QStringLiteral("cancelled"), params);
                    tm->finish(tid);
                    return;
                }
                m_sessions->appendMessage(area, QStringLiteral("assistant"), *buffer);
                m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                       QStringLiteral("done"), params,
                                       QJsonObject{{"reply", *buffer}});
                m_ws->broadcastEvent(QStringLiteral("chat.message"),
                                     QJsonObject{{"area", area},
                                                 {"role", "assistant"},
                                                 {"content", *buffer}});
                tm->finish(tid);
            };
            handlers.onError = [this, area, tid, tm, params](const QString& error) {
                // abort 触发的 "Operation canceled" 不是错误：用户主动停的
                if (tm->isCancelled(tid)) {
                    m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                           QStringLiteral("cancelled"), params);
                    tm->finish(tid);
                    return;
                }
                m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                       QStringLiteral("failed"), params,
                                       QJsonObject{{"error", error}});
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area}, {"reason", error}});
                tm->finish(tid);
            };
            m_sessions->upsertTask(tid, area, QStringLiteral("chat"),
                                   QStringLiteral("running"), params);
            m_llm->chatStream(cfg, history, {}, handlers, area); // tag = 区名，供 abortTag 定点中止
        }});

    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}

QJsonValue CoreApp::startModify(const QString& area, const QString& asset, const QString& text)
{
    // 修改锚点统一是 .blend：传 glb/fbx 时自动找同目录同名源文件
    QFileInfo info;
    if (!asset.isEmpty()) {
        info = QFileInfo(m_library->absoluteFor(asset));
        if (info.suffix().toLower() != QLatin1String("blend"))
            info = QFileInfo(info.dir().filePath(info.completeBaseName()
                                                 + QStringLiteral(".blend")));
    }
    const QString blendRelative =
        info.exists() ? m_library->relativeFor(info.absoluteFilePath()) : QString();
    if (asset.isEmpty() || text.isEmpty() || blendRelative.isEmpty()
        || !m_library->isValidTarget(blendRelative))
        return QJsonValue(QJsonObject{{"accepted", false}, {"error", "no-blend-source"}});

    BuildPlan plan;
    BuildGroup group;
    group.title = QStringLiteral("修改资产");
    group.acceptance = QStringLiteral("修改意图达成；同名 GLB/FBX/blend 已覆盖");
    plan.groups.append(group);
    plan.maxSteps = 50;
    plan.targetBase = info.completeBaseName(); // 导出回合据此覆盖同名文件
    const QString prompt = QStringLiteral("打开 .blend 源文件 %1。要求：%2。"
                                          "保留场景里已有的可导出动效（物体关键帧），不要删除或覆盖。"
                                          "过程中不要自行导出或保存；完成后由系统统一覆盖导出同名 "
                                          "GLB/FBX 并保存 .blend。")
                               .arg(info.absoluteFilePath(), text);
    const QString taskId = QStringLiteral("chat-%1").arg(++m_seq);
    const QString targetFolder = info.absolutePath();
    // 草稿已转正成最后一条 user，prompt 还会拼在末尾：历史里去掉同一条避免重复
    const QJsonArray history = historyForPrompt(area, text, 20);
    m_tasks->enqueue(
        Task{taskId, area, QString(),
             [this, plan, prompt, targetFolder, area, history](const QString& tid,
                                                               TaskManager* tm) {
                 m_orch->setModelConfig(primaryAgentConfig());
                 m_orch->setConversationHistory(history);
                 m_orch->execute(tid, tm, ToolBridge::mcpToolsToOpenAi(m_blenderTools),
                                 plan, prompt, targetFolder, area);
             }});
    return QJsonValue(QJsonObject{{"accepted", true}, {"taskId", taskId}});
}

void CoreApp::openFolderPick(const QString& area)
{
    // 清掉该区未被消费的旧 requestId，旧弹窗若仍开着再提交会得到 request-not-found
    for (auto it = m_pickRequests.begin(); it != m_pickRequests.end();) {
        if (it.value() == area)
            it = m_pickRequests.erase(it);
        else
            ++it;
    }
    const QString requestId = QStringLiteral("pick-%1").arg(++m_seq);
    m_pickRequests.insert(requestId, area);
    m_ws->broadcastEvent(QStringLiteral("folderPick.open"),
                         QJsonObject{{"area", area},
                                     {"requestId", requestId},
                                     {"rootName", "ModelLibrary"},
                                     {"tree", m_library->folderTree()}});
}
