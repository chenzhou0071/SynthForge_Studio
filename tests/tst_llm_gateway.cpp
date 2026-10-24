#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>
#include <QtTest>

#include "core/llm/LlmGateway.h"
#include "mock/MockLlmServer.h"

using namespace MockLlm;

// 约束：本文件禁止出现 `class Xxx : public LlmGateway`，也不允许回调注入绕过网络。
// 每个用例都必须让真实 chatStream 发出 POST /v1/chat/completions，
// 由 MockLlmServer 在 TCP 上回 chunked SSE。
// 有效性判据：删掉 LlmGateway.cpp 的 readyRead/finished 任一分支，这里必须变红。
class LlmGatewayTest : public QObject
{
    Q_OBJECT

private:
    struct Recorder
    {
        QStringList deltas;
        QJsonArray toolCalls;
        QStringList errors;
        int done = 0;

        LlmGateway::StreamHandlers handlers()
        {
            return {.onDelta = [this](const QString& d) { deltas << d; },
                    .onToolCalls = [this](const QJsonArray& calls) { toolCalls = calls; },
                    .onError = [this](const QString& e) { errors << e; },
                    .onDone = [this]() { ++done; }};
        }
    };

    static QJsonArray userMessages(const QString& text)
    {
        return QJsonArray{QJsonObject{{"role", "user"}, {"content", text}}};
    }

private slots:
    void emitsDeltasInOrderThenDone()
    {
        MockLlmServer server(
            {MockLlmServer::Turn{.frames = {textChunk("好"), textChunk("的"), doneChunk()},
                                 .splitEvery = 5}}); // 切点故意落在 data:/JSON/UTF-8 中间
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("你好"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.deltas, (QStringList{"好", "的"})); // 不能少、不能并成一次、不能反序
        QVERIFY(rec.errors.isEmpty());
        QCOMPARE(server.pathAt(0), QStringLiteral("/v1/chat/completions")); // baseUrl 尾斜杠已裁掉
        QCOMPARE(server.authorizationAt(0), QByteArray("Bearer test-key"));
        const QJsonObject body = server.requestObjectAt(0);
        QCOMPARE(body.value("stream").toBool(), true);
        QCOMPARE(body.value("model").toString(), QStringLiteral("test-model"));
    }

    void mergesToolCallFragmentsByIndex()
    {
        MockLlmServer server({MockLlmServer::Turn{.frames = {
                                       toolCallStartChunk(1, "call_b", "execute_blender_code",
                                                          "{\"code\":\"bpy.ops.mesh."),
                                       toolCallStartChunk(0, "call_a", "execute_blender_code",
                                                          "{\"code\":\"import bpy"),
                                       toolArgsChunk(1, "primitive_cube_add()\"}"),
                                       toolArgsChunk(0, ";bpy.ops.object.select_all()\"}"),
                                       doneChunk()},
                                   .splitEvery = 4}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("建个盒子"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.toolCalls.size(), 2); // 两个 index 各一次，不重复上报
        QVERIFY(rec.deltas.isEmpty());     // tool-only 回合不产生文本增量
        const QJsonObject first = rec.toolCalls.at(0).toObject();
        const QJsonObject second = rec.toolCalls.at(1).toObject();
        QCOMPARE(first.value("id").toString(), QStringLiteral("call_a")); // 按 index 升序还原
        QCOMPARE(second.value("id").toString(), QStringLiteral("call_b"));
        QCOMPARE(second.value("arguments").toObject().value("code").toString(),
                 QStringLiteral("bpy.ops.mesh.primitive_cube_add()")); // 分片拼完整并解析
    }

    void completesWhenFinalFrameHasNoBlankLine()
    {
        MockLlmServer server({MockLlmServer::Turn{
            .frames = {"data: {\"choices\":[{\"delta\":{\"content\":\"尾\"}}]}"}, // 无结尾 \n\n
            .splitEvery = 4}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.done == 1, 5000);
        QCOMPARE(rec.deltas, QStringList{"尾"}); // 靠 finished 里补 "\n\n" 冲出来
        QVERIFY(rec.errors.isEmpty());
    }

    void reportsErrorOnceOnHttpError()
    {
        MockLlmServer server({MockLlmServer::Turn{.status = 401}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0);
        QVERIFY(rec.deltas.isEmpty());
        QCOMPARE(server.requestCount(), 1); // 不自动重试
    }

    void reportsErrorWhenConnectionRefused()
    {
        quint16 deadPort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            deadPort = probe.serverPort();
        } // 作用域结束即关闭，端口无人监听

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(LlmConfig{QStringLiteral("http://127.0.0.1:%1/v1").arg(deadPort),
                                     QStringLiteral("test-key"), QStringLiteral("test-model")},
                           userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0); // 连接失败绝不能走 onDone
    }

    void keepsConcurrentStreamsSeparate()
    {
        MockLlmServer alpha(
            {MockLlmServer::Turn{.frames = {textChunk("alpha1"), textChunk("alpha2"), doneChunk()},
                                 .splitEvery = 5,
                                 .frameDelayMs = 15}});
        MockLlmServer beta(
            {MockLlmServer::Turn{.frames = {textChunk("beta1"), textChunk("beta2"), doneChunk()},
                                 .splitEvery = 7,
                                 .frameDelayMs = 25}});
        QVERIFY(alpha.listen());
        QVERIFY(beta.listen());

        LlmGateway gateway; // 同一个 gateway 上两条流同时在途
        Recorder a;
        Recorder b;
        gateway.chatStream(alpha.config(), userMessages("a"), {}, a.handlers());
        gateway.chatStream(beta.config(), userMessages("b"), {}, b.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(a.done + b.done == 2, 8000);
        QCOMPARE(a.deltas, (QStringList{"alpha1", "alpha2"}));
        QCOMPARE(b.deltas, (QStringList{"beta1", "beta2"}));
    }

    void abortEndsInFlightStreamsOnce()
    {
        MockLlmServer server(
            {MockLlmServer::Turn{.frames = {textChunk("半句")}, .stallAfterFrames = true}});
        QVERIFY(server.listen());

        LlmGateway gateway;
        Recorder rec;
        gateway.chatStream(server.config(), userMessages("hi"), {}, rec.handlers());

        QTRY_VERIFY_WITH_TIMEOUT(rec.deltas.size() == 1, 5000); // 首帧已到，连接仍开着
        gateway.abort();
        QTRY_VERIFY_WITH_TIMEOUT(rec.errors.size() == 1, 5000);
        QCOMPARE(rec.done, 0); // 中止不算正常完成
        gateway.abort();       // 重复 abort 不应再报错
        QTest::qWait(50);
        QCOMPARE(rec.errors.size(), 1);
    }
};

QTEST_MAIN(LlmGatewayTest)
#include "tst_llm_gateway.moc"
