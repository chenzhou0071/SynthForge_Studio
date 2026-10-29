#pragma once

#include <QObject>
#include <QString>

struct LlmConfig;
class LlmGateway;
class SessionStore;
class SettingsStore;

struct SummarizeRequest
{
    QString area;
    QString userText;
    QString mode = QStringLiteral("lowpoly");
    QString asset;
    QString discussion;
    QString shape = QStringLiteral("diorama"); // 产出形态：prop | diorama | open
};

// 总结 agent：把口语化需求改写成可编辑的专业提示词草稿（规格 5.2）。
// 独立模型配置，未单独配置时回退主 agent；草稿落库由 CoreApp 的 draftReady 接线。
class SummarizerAgent : public QObject
{
    Q_OBJECT

public:
    SummarizerAgent(LlmGateway* llm, SessionStore* sessions, SettingsStore* settings,
                    QObject* parent = nullptr);

    void generate(const SummarizeRequest& request);

signals:
    void draftReady(const QString& area, const QString& prompt);
    void failed(const QString& area, const QString& reason);

private:
    LlmConfig resolveConfig() const; // agent.summarizer 未配置 → 回退 agent.primary

    LlmGateway* m_llm;
    SessionStore* m_sessions;
    SettingsStore* m_settings;
};
