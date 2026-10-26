#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "core/agent/SummarizerAgent.h"
#include "core/llm/LlmGateway.h"
#include "core/settings/SettingsStore.h"
#include "core/store/SessionStore.h"
#include "mock/MockLlmServer.h"

namespace {

// 与设置页一致：预置服务商只存 model+key，自定义服务商才带 baseUrl
QJsonObject agentSettings(const QString& baseUrl, const QString& apiKey, const QString& model,
                          const QJsonObject& summarizer = {})
{
    QJsonObject settings = makeDefaultSettings();
    QJsonObject agent = settings.value(QStringLiteral("agent")).toObject();
    agent.insert(QStringLiteral("primary"),
                 QJsonObject{{QStringLiteral("provider"), QStringLiteral("custom")},
                             {QStringLiteral("baseUrl"), baseUrl},
                             {QStringLiteral("apiKey"), apiKey},
                             {QStringLiteral("model"), model}});
    if (!summarizer.isEmpty())
        agent.insert(QStringLiteral("summarizer"), summarizer);
    settings.insert(QStringLiteral("agent"), agent);
    return settings;
}

QJsonObject customProvider(const QString& baseUrl, const QString& apiKey, const QString& model)
{
    return QJsonObject{{QStringLiteral("provider"), QStringLiteral("custom")},
                       {QStringLiteral("baseUrl"), baseUrl},
                       {QStringLiteral("apiKey"), apiKey},
                       {QStringLiteral("model"), model}};
}

MockLlmServer::Turn textTurn(const QString& text)
{
    return MockLlmServer::Turn{.frames = {MockLlm::textChunk(text), MockLlm::doneChunk()}};
}

} // namespace

// 集成测试：总结 agent 走真实 HTTP + SSE（MockLlmServer 假服务端），
// 验证 模板渲染 → 流式收集 → 落库 → draftReady 全链，以及配置回退与错误路径。
class SummarizerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    SettingsStore m_settings{m_dir.path()};
    SessionStore m_sessions{m_dir.path() + QStringLiteral("/sessions.db")};
    LlmGateway m_llm;

private slots:
    void initTestCase() { QVERIFY(m_sessions.open()); }

    void generatesDraftFromTemplate()
    {
        MockLlmServer server({textTurn(QStringLiteral("优化后的提示词"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        QSignalSpy failed(&agent, &SummarizerAgent::failed);

        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个雨夜便利店街角场景")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(ready.at(0).at(0).toString(), QStringLiteral("blender"));
        QCOMPARE(ready.at(0).at(1).toString(), QStringLiteral("优化后的提示词"));

        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.pathAt(0), QStringLiteral("/v1/chat/completions"));
        QCOMPARE(server.authorizationAt(0), QByteArrayLiteral("Bearer k-primary"));

        const QJsonObject body = server.requestObjectAt(0);
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("primary-model"));
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        QVERIFY(!messages.isEmpty());
        const QString system =
            messages.first().toObject().value(QStringLiteral("content")).toString();
        QVERIFY2(system.contains(QStringLiteral("做一个雨夜便利店街角场景")), "模板变量未替换");
        QVERIFY2(!system.contains(QStringLiteral("{{user_request}}")), "残留 user_request 占位符");
        QVERIFY2(!system.contains(QStringLiteral("{{mode}}")), "残留 mode 占位符");
        QVERIFY2(system.contains(QStringLiteral("产出模式：lowpoly")), "mode 未注入模板");
    }

    // 主题内可以合理补充道具，只禁止与主题无关的物件
    void templateAllowsThematicEnrichment()
    {
        MockLlmServer server({textTurn(QStringLiteral("草稿"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个雨夜便利店街角场景")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QJsonArray messages =
            server.requestObjectAt(0).value(QStringLiteral("messages")).toArray();
        const QString system =
            messages.first().toObject().value(QStringLiteral("content")).toString();

        QVERIFY2(system.contains(QStringLiteral("合理补充")), "缺少主题内合理发散的许可");
        QVERIFY2(system.contains(QStringLiteral("车辆")), "车辆等街景道具不应被一刀切禁止");
        QVERIFY2(system.contains(QStringLiteral("与主题无关")), "缺少只禁无关物件的边界");
    }

    // 照明必须来自真实光源；不要默认生成飞虫、光雾、湿地面这类氛围元素
    void templateRequiresRealLightsOnly()
    {
        MockLlmServer server({textTurn(QStringLiteral("草稿"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个雨夜便利店街角场景")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QString system = server.requestObjectAt(0)
                                   .value(QStringLiteral("messages"))
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toString();

        QVERIFY2(system.contains(QStringLiteral("真实光源")), "缺少真实光源要求");
        QVERIFY2(system.contains(QStringLiteral("室内")), "缺少室内照明要求");
        QVERIFY2(system.contains(QStringLiteral("飞虫")), "缺少禁止飞虫等氛围元素的说明");
        QVERIFY2(system.contains(QStringLiteral("光粒子")), "光粒子等特效应允许保留");
        QVERIFY2(system.contains(QStringLiteral("不要默认")), "缺少默认不生成氛围元素的约束");
    }

    void summarizerConfigTakesPriority()
    {
        MockLlmServer server({textTurn(QStringLiteral("草稿"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(
            server.baseUrl(), QStringLiteral("k-primary"), QStringLiteral("primary-model"),
            customProvider(server.baseUrl(), QStringLiteral("k-sum"),
                           QStringLiteral("summary-model"))));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个路灯")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QCOMPARE(server.authorizationAt(0), QByteArrayLiteral("Bearer k-sum"));
        QCOMPARE(server.requestObjectAt(0).value(QStringLiteral("model")).toString(),
                 QStringLiteral("summary-model"));
    }

    void partialSummarizerConfigFallsBackToPrimary()
    {
        MockLlmServer server({textTurn(QStringLiteral("草稿"))});
        QVERIFY(server.listen());
        // 只填了模型没填密钥：视为未配置，整体回退主 agent
        m_settings.setData(agentSettings(
            server.baseUrl(), QStringLiteral("k-primary"), QStringLiteral("primary-model"),
            QJsonObject{{QStringLiteral("model"), QStringLiteral("summary-model")}}));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个路灯")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QCOMPARE(server.authorizationAt(0), QByteArrayLiteral("Bearer k-primary"));
        QCOMPARE(server.requestObjectAt(0).value(QStringLiteral("model")).toString(),
                 QStringLiteral("primary-model"));
    }

    void httpErrorEmitsFailed()
    {
        MockLlmServer server({MockLlmServer::Turn{
            .status = 401,
            .errorBody = QByteArrayLiteral("{\"error\":{\"message\":\"unauthorized\"}}")}});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));

        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);
        QSignalSpy failed(&agent, &SummarizerAgent::failed);

        agent.generate({QStringLiteral("blender"), QStringLiteral("做一个路灯")});

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("blender"));
        QVERIFY2(!failed.at(0).at(1).toString().isEmpty(), "失败原因为空");
    }

    void modifyModeUsesModifyTemplate()
    {
        MockLlmServer server({textTurn(QStringLiteral("把招牌材质换成深绿做旧铁皮，粗糙度调高"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));
        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);

        agent.generate({QStringLiteral("blender"), QStringLiteral("招牌弄旧一点"),
                        QStringLiteral("modify"), QStringLiteral("sword_01"), {}});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QString system = server.requestObjectAt(0)
                                   .value(QStringLiteral("messages"))
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toString();
        QVERIFY2(system.contains(QStringLiteral("已经存在")), "没有使用修改模板");
        QVERIFY2(!system.contains(QStringLiteral("底座与构图")), "串进了新建模板的骨架");
        QVERIFY2(system.contains(QStringLiteral("sword_01")), "资产名未注入");
    }

    void discussionIsInjected()
    {
        MockLlmServer server({textTurn(QStringLiteral("主题与视角：雨夜便利店"))});
        QVERIFY(server.listen());
        m_settings.setData(agentSettings(server.baseUrl(), QStringLiteral("k-primary"),
                                         QStringLiteral("primary-model")));
        SummarizerAgent agent(&m_llm, &m_sessions, &m_settings);
        QSignalSpy ready(&agent, &SummarizerAgent::draftReady);

        agent.generate({QStringLiteral("blender"), QStringLiteral("做个便利店"),
                        QStringLiteral("lowpoly"), {},
                        QStringLiteral("已确认：要有自动门与积水")});

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        const QString system = server.requestObjectAt(0)
                                   .value(QStringLiteral("messages"))
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toString();
        QVERIFY2(system.contains(QStringLiteral("用户已确认的讨论要点")), "缺讨论结论段");
        QVERIFY2(system.contains(QStringLiteral("自动门与积水")), "讨论结论未注入");
    }
};

QTEST_MAIN(SummarizerTest)
#include "tst_summarizer.moc"
