#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "core/agent/DiscussionAgent.h"
#include "core/llm/LlmGateway.h"
#include "mock/MockLlmServer.h"
#include "mock/MockMcpHarness.h"

namespace {

MockLlmServer::Turn textTurn(const QString& text)
{
    return MockLlmServer::Turn{.frames = {MockLlm::textChunk(text), MockLlm::doneChunk()}};
}

MockLlmServer::Turn toolTurn(const QString& name)
{
    return MockLlmServer::Turn{.frames = {MockLlm::toolCallChunk(name), MockLlm::doneChunk()}};
}

QJsonArray mcpToolList()
{
    auto entry = [](const char* name) {
        return QJsonObject{{QStringLiteral("name"), QLatin1String(name)},
                           {QStringLiteral("description"), QLatin1String(name)},
                           {QStringLiteral("inputSchema"),
                            QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}};
    };
    return QJsonArray{entry("execute_blender_code"), entry("get_scene_info"), entry("look"),
                      entry("import_asset"),         entry("generate_3d"),   entry("search_assets")};
}

} // namespace

class DiscussionTest : public QObject
{
    Q_OBJECT

private slots:
    void toolWhitelistKeepsOnlyReadOnlySemantics()
    {
        const QJsonArray tools = discussionTools(mcpToolList());
        QStringList names;
        for (const QJsonValue& value : tools)
            names << value.toObject().value(QStringLiteral("function"))
                       .toObject()
                       .value(QStringLiteral("name"))
                       .toString();
        QVERIFY(names.contains(QStringLiteral("get_scene_info")));
        QVERIFY(names.contains(QStringLiteral("look")));
        // 讨论给模型「手」就废了整条「草稿→确认→执行」边界
        QVERIFY(!names.contains(QStringLiteral("execute_blender_code")));
        QVERIFY(!names.contains(QStringLiteral("import_asset")));
        QVERIFY(!names.contains(QStringLiteral("generate_3d")));
    }

    void inspectObjectIsAVirtualToolBackedByFixedScript()
    {
        // 模型只表达「我想看哪个对象」，脚本由后端固定拼：能读参数，不能改场景
        QVERIFY(inspectObjectScript(QStringLiteral("desk_top")).contains(QStringLiteral("bpy.data.objects.get")));
        QVERIFY(inspectObjectScript(QStringLiteral("desk_top")).contains(QStringLiteral("dimensions")));
        QVERIFY(inspectObjectScript(QStringLiteral("desk_top")).contains(QStringLiteral("materials")));
        QVERIFY(inspectObjectScript(QStringLiteral("desk_top")).contains(QStringLiteral("action")));
        // 只读：不许出现写操作
        const QString script = inspectObjectScript(QStringLiteral("desk_top"));
        QVERIFY(!script.contains(QStringLiteral("object.delete")));
        QVERIFY(!script.contains(QStringLiteral("location =")));
        QVERIFY(!script.contains(QStringLiteral("save_")));
    }

    void unsafeObjectNamesAreRejected()
    {
        QVERIFY(isSafeObjectName(QStringLiteral("desk_top")));
        QVERIFY(isSafeObjectName(QStringLiteral("mug_01")));
        // 名字要进 Python 字面量：引号、反斜杠、换行、分号一律拒
        QVERIFY(!isSafeObjectName(QStringLiteral("a'b")));
        QVERIFY(!isSafeObjectName(QStringLiteral("a\"b")));
        QVERIFY(!isSafeObjectName(QStringLiteral("a\\b")));
        QVERIFY(!isSafeObjectName(QStringLiteral("a\nb")));
        QVERIFY(!isSafeObjectName(QStringLiteral("a;print(1)")));
        QVERIFY(!isSafeObjectName(QString()));
        QVERIFY(!isSafeObjectName(QStringLiteral("x").repeated(200)));
    }

    void askWithToolsRunsOneReadOnlyInspectionThenAnswers()
    {
        MockLlmServer server({toolTurn(QStringLiteral("inspect_object")),
                              textTurn(QStringLiteral("这张书桌桌面 1.4×0.7 米，可以加个抽屉。"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        DiscussionAgent agent(&llm, &mcp.client());
        agent.setModelConfig(server.config());
        agent.setMcpTools(mcpToolList());
        QSignalSpy finished(&agent, &DiscussionAgent::finished);
        QSignalSpy steps(&agent, &DiscussionAgent::toolStep);

        agent.ask(QStringLiteral("blender"), QStringLiteral("系统提示"), QJsonArray{},
                  QStringLiteral("桌面多宽？"), true);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(steps.at(0).at(1).toString(), QStringLiteral("inspect_object"));
        // 走的是 execute_blender_code，但对模型只暴露 inspect_object
        QVERIFY(mcp.calls.contains(QStringLiteral("execute_blender_code")));
        QVERIFY(finished.at(0).at(1).toString().contains(QStringLiteral("1.4")));
    }

    void askWithoutToolsSendsNoToolList()
    {
        // 未连接或 Blender 忙：tools 传空，退回纯文字讨论，不报错打断聊天
        MockLlmServer server({textTurn(QStringLiteral("只能凭文字聊。"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        DiscussionAgent agent(&llm, &mcp.client());
        agent.setModelConfig(server.config());
        QSignalSpy finished(&agent, &DiscussionAgent::finished);

        agent.ask(QStringLiteral("blender"), QStringLiteral("系统提示"), QJsonArray{},
                  QStringLiteral("桌面多宽？"), false);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(server.requestCount(), 1);
        QVERIFY(!server.requestAt(0).contains(QStringLiteral("tools")));
        QCOMPARE(mcp.calls.size(), 0);
    }

    void toolCallsAreCappedAtSixAndWrapUp()
    {
        QList<MockLlmServer::Turn> turns;
        for (int i = 0; i < 8; ++i)
            turns << toolTurn(QStringLiteral("look"));
        turns << textTurn(QStringLiteral("看完了，说结论。"));
        MockLlmServer server(turns);
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        DiscussionAgent agent(&llm, &mcp.client());
        agent.setModelConfig(server.config());
        agent.setMcpTools(mcpToolList());
        QSignalSpy finished(&agent, &DiscussionAgent::finished);
        QSignalSpy steps(&agent, &DiscussionAgent::toolStep);

        agent.ask(QStringLiteral("blender"), QStringLiteral("系统"), QJsonArray{},
                  QStringLiteral("再看看"), true);

        // 不封顶的话模型能「我再看一眼」看到天黑，用户等一分钟出不来一句话
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        int running = 0;
        for (const QVariantList& args : steps) {
            if (args.at(2).toString() == QLatin1String("running"))
                ++running;
        }
        QCOMPARE(running, 6); // 每个调用有 running + done 两条，只数发起的
        // 封顶后不再回喂：收尾语必须出现，且不能双发 finished
        QVERIFY(finished.at(0).at(1).toString().contains(QStringLiteral("查看次数已达上限")));
        QCOMPARE(finished.count(), 1);
    }

    void execBlockStaysOutOfStreamedText()
    {
        MockLlmServer server({textTurn(QStringLiteral("建议加高护栏。[[EXEC]]把护栏加到 1.1 米"))});
        QVERIFY(server.listen());
        LlmGateway llm;
        MockMcpHarness mcp;
        DiscussionAgent agent(&llm, &mcp.client());
        agent.setModelConfig(server.config());
        QSignalSpy finished(&agent, &DiscussionAgent::finished);
        QSignalSpy deltas(&agent, &DiscussionAgent::delta);

        agent.ask(QStringLiteral("blender"), QStringLiteral("系统"), QJsonArray{},
                  QStringLiteral("护栏多高合适？"), false);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QString streamed;
        for (const QVariantList& args : deltas)
            streamed += args.at(1).toString();
        // 草稿留在服务端：流式里出现 [[EXEC]] 或草稿正文就是泄漏
        QVERIFY(!streamed.contains(QStringLiteral("[[EXEC]]")));
        QVERIFY(!streamed.contains(QStringLiteral("把护栏加到 1.1 米")));
        QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("把护栏加到 1.1 米"));
    }
};

QTEST_GUILESS_MAIN(DiscussionTest)
#include "tst_discussion.moc"
