#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class SettingsStore;
class SessionStore;
class TaskManager;
class ProtocolRouter;
class WsServer;
class LlmGateway;

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

    CoreOptions m_options;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<SessionStore> m_sessions;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<ProtocolRouter> m_router;
    std::unique_ptr<WsServer> m_ws;
    std::unique_ptr<LlmGateway> m_llm;
    QHash<QString, QJsonObject> m_lastSend;     // area -> 最近一次 chat.send 参数
    QHash<QString, RestartFn> m_bridgeRestarts; // area -> 桥接重启回调
    qint64 m_seq = 0;
};
