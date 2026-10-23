#include "core/coreapp/CoreApp.h"

#include "core/llm/LlmGateway.h"
#include "core/settings/SettingsStore.h"
#include "core/store/SessionStore.h"
#include "core/tasks/TaskManager.h"
#include "core/ws/ProtocolRouter.h"
#include "core/ws/WsServer.h"

#include <QJsonObject>
#include <QJsonValue>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

// 预置服务商只存 model + key，baseUrl 从 providerBaseUrls 表取；自定义服务商才带 baseUrl
QString resolveBaseUrl(const QJsonObject& primary, const QJsonObject& settings)
{
    const QString explicitUrl = primary.value(QStringLiteral("baseUrl")).toString();
    if (!explicitUrl.isEmpty())
        return explicitUrl;
    return settings.value(QStringLiteral("providerBaseUrls"))
        .toObject()
        .value(primary.value(QStringLiteral("provider")).toString())
        .toString();
}

} // namespace

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
    return true;
}

void CoreApp::registerBridge(const QString& area, RestartFn fn)
{
    m_bridgeRestarts.insert(area, std::move(fn));
}

QJsonValue CoreApp::handleChatSend(const QJsonObject& params, bool appendUser)
{
    const QString area = params.value(QStringLiteral("area")).toString();
    const QString text = params.value(QStringLiteral("text")).toString();
    m_lastSend.insert(area, params);

    if (appendUser) {
        m_sessions->appendMessage(area, QStringLiteral("user"), text);
        m_ws->broadcastEvent(QStringLiteral("chat.message"),
                             QJsonObject{{"area", area}, {"role", "user"}, {"content", text}});
    }

    const QJsonObject data = m_settings->data();
    const QJsonObject primary = data.value(QStringLiteral("agent"))
                                   .toObject()
                                   .value(QStringLiteral("primary"))
                                   .toObject();
    const QString baseUrl = resolveBaseUrl(primary, data);
    const QString apiKey = primary.value(QStringLiteral("apiKey")).toString();
    const QString model = primary.value(QStringLiteral("model")).toString();
    if (apiKey.isEmpty() || model.isEmpty() || baseUrl.isEmpty()) {
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

    const LlmConfig cfg{baseUrl, apiKey, model};
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
