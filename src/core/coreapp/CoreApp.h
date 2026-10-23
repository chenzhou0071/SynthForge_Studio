#pragma once

#include <QObject>
#include <QString>

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

private:
    CoreOptions m_options;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<SessionStore> m_sessions;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<ProtocolRouter> m_router;
    std::unique_ptr<WsServer> m_ws;
    std::unique_ptr<LlmGateway> m_llm;
    qint64 m_seq = 0;
};
