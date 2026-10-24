#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

#include "core/llm/LlmGateway.h"
#include "core/mcp/BlenderGate.h"

class SettingsStore;
class SessionStore;
class TaskManager;
class ProtocolRouter;
class WsServer;
class McpRuntime;
class PreflightChecker;
class SummarizerAgent;

struct CoreOptions
{
    QString dataDir;
    bool headless = false;
};

class CoreApp : public QObject
{
    Q_OBJECT

public:
    explicit CoreApp(const CoreOptions& options, QObject* parent = nullptr);
    ~CoreApp() override;

    bool start();
    WsServer* ws() const { return m_ws.get(); }

    using RestartFn = std::function<bool()>;
    // 由 P4（blender）/ P6（unity）注入桥接重启回调
    void registerBridge(const QString& area, RestartFn fn);

private:
    QJsonValue handleChatSend(const QJsonObject& params, bool appendUser = true);
    void broadcastConnStatus(const QString& area, const QString& status, const QString& hint);
    LlmConfig primaryAgentConfig() const;

    CoreOptions m_options;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<SessionStore> m_sessions;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<ProtocolRouter> m_router;
    std::unique_ptr<WsServer> m_ws;
    std::unique_ptr<LlmGateway> m_llm;
    McpRuntime* m_blender = nullptr; // QObject child，析构由 parent 负责
    std::unique_ptr<PreflightChecker> m_preflight;
    SummarizerAgent* m_summarizer = nullptr; // QObject child，析构由 parent 负责
    BlenderGate m_blenderGate; // Blender agent 任务与 unity 转换互斥
    QJsonArray m_blenderTools;
    QHash<QString, QString> m_status; // area -> off|reconnecting|ready
    QHash<QString, QString> m_hints;  // area -> 状态提示文案
    QSet<QString> m_everReady;        // 曾握手成功的区：之后的 off 才算桥接崩溃
    QSet<QString> m_crashNotified;    // 本轮断链已出过卡，恢复 ready 时清除
    QHash<QString, QJsonObject> m_lastSend;     // area -> 最近一次 chat.send 参数
    QHash<QString, RestartFn> m_bridgeRestarts; // area -> 桥接重启回调
    qint64 m_seq = 0;
};
