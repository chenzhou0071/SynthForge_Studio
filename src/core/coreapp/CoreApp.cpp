#include "core/coreapp/CoreApp.h"

#include "core/agent/AgentOrchestrator.h"
#include "core/agent/SummarizerAgent.h"
#include "core/llm/LlmGateway.h"
#include "core/mcp/BridgeCommands.h"
#include "core/mcp/McpRuntime.h"
#include "core/mcp/StdioTransport.h"
#include "core/preflight/PreflightChecker.h"
#include "core/settings/SettingsStore.h"
#include "core/store/SessionStore.h"
#include "core/tasks/TaskManager.h"
#include "core/ws/ProtocolRouter.h"
#include "core/ws/WsServer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

CoreApp::CoreApp(const CoreOptions& options, QObject* parent)
    : QObject(parent)
    , m_options(options)
    , m_settings(std::make_unique<SettingsStore>(options.dataDir))
    , m_sessions(
          std::make_unique<SessionStore>(options.dataDir + QStringLiteral("/sessions.db")))
    , m_tasks(std::make_unique<TaskManager>())
    , m_router(std::make_unique<ProtocolRouter>())
    , m_ws(std::make_unique<WsServer>(m_router.get()))
    , m_llm(std::make_unique<LlmGateway>())
{
}

CoreApp::~CoreApp() = default;

bool CoreApp::start()
{
    m_settings->load();
    if (!m_sessions->open())
        return false;

    // Blender 桥接运行时：先建好再注册 handler，保证 chat.send 的前置检查有可用对象
    m_blender = new McpRuntime(
        QStringLiteral("blender"),
        std::make_unique<StdioTransport>(defaultBridgeCommand(QStringLiteral("blender"))),
        this);
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
                m_ws->broadcastEvent(
                    QStringLiteral("draft.created"),
                    QJsonObject{{"area", area},
                                {"taskId", QStringLiteral("draft-%1").arg(++m_seq)},
                                {"prompt", prompt}});
            });
    connect(m_summarizer, &SummarizerAgent::failed, this,
            [this](const QString& area, const QString& reason) {
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area}, {"reason", reason}});
            });

    // 编排器：计划卡与分组执行。plan() 由 draft.send 触发（Task 7），
    // plan.confirm / plan.cancel 在 Task 4 接线；本轮先把信号接到前端。
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
    connect(m_orch, &AgentOrchestrator::finalText, this,
            [this](const QString& area, const QString& text) {
                m_sessions->appendMessage(area, QStringLiteral("assistant"), text);
                m_ws->broadcastEvent(QStringLiteral("chat.message"),
                                     QJsonObject{{"area", area},
                                                 {"role", "assistant"},
                                                 {"content", text}});
            });
    connect(m_orch, &AgentOrchestrator::failed, this,
            [this](const QString& area, const QString& reason) {
                m_ws->broadcastEvent(QStringLiteral("error.card"),
                                     QJsonObject{{"area", area}, {"reason", reason}});
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
                m_pendingPlans.insert(area, plan);
                m_ws->broadcastEvent(QStringLiteral("plan.created"),
                                     QJsonObject{{"area", area},
                                                 {"taskId", taskId},
                                                 {"groups", plan.value("groups")},
                                                 {"totalSteps", plan.value("totalSteps")}});
            });
    connect(m_orch, &AgentOrchestrator::artifactsReady, this,
            [this](const QString& area, const QStringList& absoluteGlbs) {
                // Task 5 接管：转相对路径 + asset.created 增量。当前只通知前端刷新树
                if (area != QLatin1String("blender") || absoluteGlbs.isEmpty())
                    return;
                m_ws->broadcastEvent(QStringLiteral("fileTree.changed"),
                                     QJsonObject{{"area", area},
                                                 {"reason", "full"},
                                                 {"paths", QJsonArray{}}});
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
    m_router->registerHandler(QStringLiteral("draft.send"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  const QString prompt =
                                      params.value(QStringLiteral("prompt")).toString().trimmed();
                                  if (area.isEmpty() || prompt.isEmpty())
                                      return QJsonValue(QJsonObject{
                                          {"accepted", false}, {"reason", "invalid-draft"}});
                                  // 草稿确认后成为正式指令：落库 + 进历史（规格 5.2）；
                                  // 规划阶段由 AgentOrchestrator 接管（Task 3）
                                  m_sessions->saveDraft(area, prompt);
                                  m_sessions->appendMessage(area, QStringLiteral("user"), prompt);
                                  m_ws->broadcastEvent(
                                      QStringLiteral("chat.message"),
                                      QJsonObject{{"area", area},
                                                  {"role", "user"},
                                                  {"content", prompt}});
                                  return QJsonValue(QJsonObject{{"accepted", true}});
                              });
    m_router->registerHandler(QStringLiteral("draft.cancel"),
                              [this](const QJsonObject& params) {
                                  const QString area =
                                      params.value(QStringLiteral("area")).toString();
                                  // content 列 NOT NULL：清空用空串，不能传 null QString
                                  if (!area.isEmpty())
                                      m_sessions->saveDraft(area, QStringLiteral(""));
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
                                  return QJsonValue(QJsonObject{{"ok", true}});
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

    QJsonObject hints;
    for (auto it = m_hints.constBegin(); it != m_hints.constEnd(); ++it)
        hints.insert(it.key(), it.value());

    m_ws->broadcastEvent(QStringLiteral("conn.status"),
                         QJsonObject{{"blender", m_status.value(QStringLiteral("blender"), "off")},
                                     {"unity", m_status.value(QStringLiteral("unity"), "off")},
                                     {"hints", hints}});

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

QJsonValue CoreApp::handleChatSend(const QJsonObject& params, bool appendUser)
{
    const QString area = params.value(QStringLiteral("area")).toString();
    const QString text = params.value(QStringLiteral("text")).toString();
    const QString mode = params.value(QStringLiteral("mode")).toString(); // 空 = P2 通用流

    // 前置检查先于任何落库/LLM 调用：链未就绪直接拒绝，不浪费总结/建模调用
    if (mode == QLatin1String("lowpoly") || mode == QLatin1String("cloud3d")
        || mode == QLatin1String("modify")) {
        const PreflightResult pre = mode == QLatin1String("cloud3d")
                                        ? m_preflight->checkCloud3d()
                                        : m_preflight->checkLowpoly();
        if (!pre.ok) {
            m_ws->broadcastEvent(QStringLiteral("error.card"),
                                 QJsonObject{{"area", area}, {"reason", pre.reason}});
            return QJsonValue(QJsonObject{{"accepted", false}});
        }
    }

    m_lastSend.insert(area, params);

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
        m_summarizer->generate(area, text); // 草稿经 draft.created 下发
        return QJsonValue(QJsonObject{{"accepted", true}});
    }

    if (mode == QLatin1String("cloud3d")) {
        if (appendUser) {
            m_sessions->appendMessage(area, QStringLiteral("user"), text);
            m_ws->broadcastEvent(
                QStringLiteral("chat.message"),
                QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
        }
        // P5 接管云 API 生成：先占位拒绝，避免落到通用流
        m_ws->broadcastEvent(QStringLiteral("error.card"),
                             QJsonObject{{"area", area}, {"reason", "cloud3d-not-implemented"}});
        return QJsonValue(QJsonObject{{"accepted", false},
                                      {"reason", "cloud3d-not-implemented"}});
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
    QJsonArray history;
    for (const QJsonValue& value : m_sessions->messagePage(area, 20).messages) {
        const QJsonObject message = value.toObject();
        history.append(QJsonObject{{"role", message.value(QStringLiteral("role"))},
                                   {"content", message.value(QStringLiteral("content"))}});
    }

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
