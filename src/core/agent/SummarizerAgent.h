#pragma once

#include <QObject>
#include <QString>

struct LlmConfig;
class LlmGateway;
class SessionStore;
class SettingsStore;

// 总结 agent：把口语化需求改写成可编辑的专业提示词草稿（规格 5.2）。
// 独立模型配置，未单独配置时回退主 agent；草稿落库后由 CoreApp 广播 draft.created。
class SummarizerAgent : public QObject
{
    Q_OBJECT

public:
    SummarizerAgent(LlmGateway* llm, SessionStore* sessions, SettingsStore* settings,
                    QObject* parent = nullptr);

    void generate(const QString& area, const QString& userText,
                  const QString& mode = QStringLiteral("lowpoly"));

signals:
    void draftReady(const QString& area, const QString& prompt);
    void failed(const QString& area, const QString& reason);

private:
    LlmConfig resolveConfig() const; // agent.summarizer 未配置 → 回退 agent.primary

    LlmGateway* m_llm;
    SessionStore* m_sessions;
    SettingsStore* m_settings;
};
