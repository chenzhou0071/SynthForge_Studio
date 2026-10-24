#include "core/agent/SummarizerAgent.h"

#include "core/llm/LlmGateway.h"
#include "core/settings/SettingsStore.h"
#include "core/store/SessionStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include <memory>

namespace {

// 资源编在静态库里：显式初始化，避免 rcc 生成的目标文件因无符号引用而不被链接进来
void ensurePromptResources()
{
    static const bool once = [] {
        Q_INIT_RESOURCE(prompts);
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace

SummarizerAgent::SummarizerAgent(LlmGateway* llm, SessionStore* sessions,
                                 SettingsStore* settings, QObject* parent)
    : QObject(parent)
    , m_llm(llm)
    , m_sessions(sessions)
    , m_settings(settings)
{
    ensurePromptResources();
}

LlmConfig SummarizerAgent::resolveConfig() const
{
    const QJsonObject data = m_settings->data();
    const QJsonObject agent = data.value(QStringLiteral("agent")).toObject();
    QJsonObject picked = agent.value(QStringLiteral("summarizer")).toObject();
    // 缺密钥或模型视为「未单独配置」：整组回退主 agent，不混用两套服务商
    if (picked.value(QStringLiteral("apiKey")).toString().isEmpty()
        || picked.value(QStringLiteral("model")).toString().isEmpty()) {
        picked = agent.value(QStringLiteral("primary")).toObject();
    }
    return LlmConfig{resolveAgentBaseUrl(picked, data),
                     picked.value(QStringLiteral("apiKey")).toString(),
                     picked.value(QStringLiteral("model")).toString()};
}

void SummarizerAgent::generate(const QString& area, const QString& userText, const QString& mode)
{
    ensurePromptResources();

    const LlmConfig cfg = resolveConfig();
    if (cfg.baseUrl.isEmpty() || cfg.apiKey.isEmpty() || cfg.model.isEmpty()) {
        emit failed(area, QStringLiteral("agent-not-configured"));
        return;
    }

    const QString templatePath = mode == QLatin1String("cloud3d")
                                     ? QStringLiteral(":/prompts/summarizer-cloud3d.md")
                                     : QStringLiteral(":/prompts/summarizer.md");
    QFile tpl(templatePath);
    QString system;
    if (tpl.open(QIODevice::ReadOnly | QIODevice::Text))
        system = QString::fromUtf8(tpl.readAll());
    if (system.isEmpty()) {
        emit failed(area, QStringLiteral("prompt-template-missing"));
        return;
    }
    system.replace(QStringLiteral("{{user_request}}"), userText);
    system.replace(QStringLiteral("{{mode}}"), mode);

    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"), system}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), userText}}};

    // 缓冲放堆上：onDone / onError 可能在 chatStream 返回之后才触发
    auto buffer = std::make_shared<QString>();
    m_llm->chatStream(
        cfg, messages, {},
        {.onDelta = [buffer](const QString& delta) { buffer->append(delta); },
         .onToolCalls = nullptr,
         .onError = [this, area](const QString& error) { emit failed(area, error); },
         .onDone = [this, area, buffer]() {
             m_sessions->saveDraft(area, *buffer);
             emit draftReady(area, *buffer);
         }});
}
