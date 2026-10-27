#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QUrl>

#include "core/cloud3d/CustomProvider.h"
#include "core/cloud3d/HunyuanProvider.h"
#include "core/cloud3d/Tc3Signature.h"
#include "core/cloud3d/TripoProvider.h"
#include "mock/MockCloud3dServer.h"

namespace {
QByteArray headerValue(const QByteArray& head, const QByteArray& name)
{
    for (const QByteArray& line : head.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
            return line.mid(colon + 1).trimmed();
    }
    return {};
}

Cloud3dStatus statusAt(const QSignalSpy& spy, int index)
{
    return qvariant_cast<Cloud3dStatus>(spy.at(index).at(0));
}

QByteArray json(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
} // namespace

class Cloud3dTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<Cloud3dStatus>("Cloud3dStatus"); }

    void tripoSubmitPostsPromptAndEmitsTaskId()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/v2/openapi/task", 200,
                       json({{"code", 0}, {"data", QJsonObject{{"task_id", "t1"}}}}));

        TripoProvider provider("tripo-key", "v2.5-20250123", server.baseUrl());
        QSignalSpy submitted(&provider, &TripoProvider::submitted);
        provider.submit("a wooden crate", QString());

        QTRY_COMPARE(submitted.count(), 1);
        QCOMPARE(submitted.first().at(0).toString(), QStringLiteral("t1"));
        QCOMPARE(server.requests().size(), 1);

        const MockCloud3dServer::Request request = server.requests().first();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.path, QStringLiteral("/v2/openapi/task"));
        QCOMPARE(headerValue(request.head, "authorization"), QByteArray("Bearer tripo-key"));

        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        QCOMPARE(body.value("type").toString(), QStringLiteral("text_to_model"));
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a wooden crate"));
        QCOMPARE(body.value("model_version").toString(), QStringLiteral("v2.5-20250123"));
    }

    void tripoPollMapsStatusAndModelUrl()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        const QString modelUrl = QStringLiteral("http://example.test/t1.glb");
        server.enqueue("GET", "/v2/openapi/task/t1", 200,
                       json({{"code", 0},
                             {"data", QJsonObject{{"status", "running"}, {"progress", 42}}}}));
        server.enqueue("GET", "/v2/openapi/task/t1", 200,
                       json({{"code", 0},
                             {"data",
                              QJsonObject{{"status", "success"},
                                          {"progress", 100},
                                          {"output",
                                           QJsonObject{{"p",
                                                        QJsonObject{{"url", modelUrl}}}}}}}}));

        TripoProvider provider("tripo-key", QString(), server.baseUrl());
        QSignalSpy status(&provider, &TripoProvider::status);

        provider.poll("t1");
        QTRY_COMPARE(status.count(), 1);
        const Cloud3dStatus running = statusAt(status, 0);
        QCOMPARE(running.state, QStringLiteral("running"));
        QCOMPARE(running.percent, 42);

        provider.poll("t1");
        QTRY_COMPARE(status.count(), 2);
        const Cloud3dStatus success = statusAt(status, 1);
        QCOMPARE(success.state, QStringLiteral("success"));
        QCOMPARE(success.percent, 100);
        QCOMPARE(success.modelUrl, QStringLiteral("http://example.test/t1.glb"));
        QCOMPARE(headerValue(server.requests().at(1).head, "authorization"),
                 QByteArray("Bearer tripo-key"));
    }

    void tripoBusinessErrorSurfacesServerMessage()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/v2/openapi/task", 401,
                       json({{"code", 1004}, {"message", "invalid api key"}}));

        TripoProvider provider("bad-key", QString(), server.baseUrl());
        QSignalSpy failed(&provider, &TripoProvider::failed);
        provider.submit("a wooden crate", QString());

        QTRY_COMPARE(failed.count(), 1);
        const QString reason = failed.first().at(0).toString();
        QVERIFY(reason.contains(QStringLiteral("invalid api key")));
        QVERIFY(!reason.startsWith(QStringLiteral("retryable:")));
    }

    void tripoNetworkErrorIsRetryable()
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close(); // 端口空出来，连接会被立即拒绝

        TripoProvider provider("key", QString(),
                               QStringLiteral("http://127.0.0.1:%1").arg(port));
        QSignalSpy failed(&provider, &TripoProvider::failed);
        provider.submit("a wooden crate", QString());

        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().at(0).toString().startsWith(QStringLiteral("retryable:")));
    }

    void customSubmitPostsPrompt()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/task", 200, json({{"task_id", "c1"}}));

        CustomProvider provider(server.baseUrl() + QStringLiteral("/"), "custom-token");
        QSignalSpy submitted(&provider, &CustomProvider::submitted);
        provider.submit("a rusty sign", QString());

        QTRY_COMPARE(submitted.count(), 1);
        QCOMPARE(submitted.first().at(0).toString(), QStringLiteral("c1"));

        const MockCloud3dServer::Request request = server.requests().first();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.path, QStringLiteral("/task"));
        QCOMPARE(headerValue(request.head, "authorization"), QByteArray("Bearer custom-token"));
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        QCOMPARE(body.value("prompt").toString(), QStringLiteral("a rusty sign"));
    }

    void customPollMapsSuccess()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        const QString modelUrl = QStringLiteral("http://example.test/c1.glb");
        server.enqueue("GET", "/task/c1", 200,
                       json({{"status", "success"}, {"progress", 100}, {"model_url", modelUrl}}));

        CustomProvider provider(server.baseUrl(), QString());
        QSignalSpy status(&provider, &CustomProvider::status);
        provider.poll("c1");

        QTRY_COMPARE(status.count(), 1);
        const Cloud3dStatus result = statusAt(status, 0);
        QCOMPARE(result.state, QStringLiteral("success"));
        QCOMPARE(result.percent, 100);
        QCOMPARE(result.modelUrl, QStringLiteral("http://example.test/c1.glb"));
        QVERIFY(headerValue(server.requests().first().head, "authorization").isEmpty());
    }

    void customFailedStatusCarriesError()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("GET", "/task/c1", 200,
                       json({{"status", "failed"}, {"progress", 30}, {"error", "quota exceeded"}}));

        CustomProvider provider(server.baseUrl(), QString());
        QSignalSpy status(&provider, &CustomProvider::status);
        provider.poll("c1");

        QTRY_COMPARE(status.count(), 1);
        const Cloud3dStatus result = statusAt(status, 0);
        QCOMPARE(result.state, QStringLiteral("failed"));
        QCOMPARE(result.error, QStringLiteral("quota exceeded"));
    }

    void hunyuanSubmitSignsAndEmitsJobId()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/", 200,
                       json({{"Response",
                              QJsonObject{{"JobId", "j1"}, {"RequestId", "r1"}}}}));

        HunyuanProvider provider("AKIDEXAMPLE", "SECRETKEY", "ap-guangzhou", QString(),
                                 server.baseUrl());
        QSignalSpy submitted(&provider, &HunyuanProvider::submitted);
        provider.submit("a wooden crate", QString());

        QTRY_COMPARE(submitted.count(), 1);
        QCOMPARE(submitted.first().at(0).toString(), QStringLiteral("j1"));

        const MockCloud3dServer::Request request = server.requests().first();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.path, QStringLiteral("/"));
        QCOMPARE(headerValue(request.head, "x-tc-action"), QByteArray("SubmitHunyuanTo3DJob"));
        QCOMPARE(headerValue(request.head, "x-tc-version"), QByteArray("2025-05-13"));
        QCOMPARE(headerValue(request.head, "x-tc-region"), QByteArray("ap-guangzhou"));
        QCOMPARE(headerValue(request.head, "content-type"),
                 QByteArray("application/json; charset=utf-8"));

        const qint64 timestamp = headerValue(request.head, "x-tc-timestamp").toLongLong();
        QVERIFY(timestamp > 0);
        const QString host = QUrl(server.baseUrl()).authority();
        const QString expected = tc3Authorization("AKIDEXAMPLE", "SECRETKEY", "ai3d", host,
                                                  request.body, timestamp);
        QCOMPARE(headerValue(request.head, "authorization"), expected.toUtf8());

        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        QCOMPARE(body.value("Prompt").toString(), QStringLiteral("a wooden crate"));
        QCOMPARE(body.value("ResultFormat").toString(), QStringLiteral("GLB"));
    }

    void hunyuanPollMapsStatusAndPicksGlb()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/", 200,
                       json({{"Response", QJsonObject{{"Status", "WAIT"}}}}));
        server.enqueue("POST", "/", 200,
                       json({{"Response", QJsonObject{{"Status", "RUN"}}}}));
        const QString modelUrl = QStringLiteral("http://example.test/a.glb");
        QJsonArray files;
        files.append(QJsonObject{{"Type", "OBJ"}, {"Url", "http://example.test/a.obj"}});
        files.append(QJsonObject{{"Type", "GLB"}, {"Url", modelUrl}});
        server.enqueue("POST", "/", 200,
                       json({{"Response",
                              QJsonObject{{"Status", "DONE"}, {"ResultFile3Ds", files}}}}));

        HunyuanProvider provider("AKIDEXAMPLE", "SECRETKEY", "ap-guangzhou", QString(),
                                 server.baseUrl());
        QSignalSpy status(&provider, &HunyuanProvider::status);

        provider.poll("j1");
        QTRY_COMPARE(status.count(), 1);
        QCOMPARE(statusAt(status, 0).state, QStringLiteral("queued"));
        QCOMPARE(statusAt(status, 0).percent, 0);

        provider.poll("j1");
        QTRY_COMPARE(status.count(), 2);
        QCOMPARE(statusAt(status, 1).state, QStringLiteral("running"));
        QCOMPARE(statusAt(status, 1).percent, 50);

        provider.poll("j1");
        QTRY_COMPARE(status.count(), 3);
        const Cloud3dStatus done = statusAt(status, 2);
        QCOMPARE(done.state, QStringLiteral("success"));
        QCOMPARE(done.percent, 100);
        QCOMPARE(done.modelUrl, modelUrl);

        const MockCloud3dServer::Request request = server.requests().at(2);
        QCOMPARE(headerValue(request.head, "x-tc-action"), QByteArray("QueryHunyuanTo3DJob"));
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        QCOMPARE(body.value("JobId").toString(), QStringLiteral("j1"));
    }

    void hunyuanFailStatusCarriesError()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/", 200,
                       json({{"Response",
                              QJsonObject{{"Status", "FAIL"},
                                          {"ErrorCode", "FailedOperation"},
                                          {"ErrorMessage", "quota exceeded"}}}}));

        HunyuanProvider provider("AKIDEXAMPLE", "SECRETKEY", "ap-guangzhou", QString(),
                                 server.baseUrl());
        QSignalSpy status(&provider, &HunyuanProvider::status);
        provider.poll("j1");

        QTRY_COMPARE(status.count(), 1);
        const Cloud3dStatus result = statusAt(status, 0);
        QCOMPARE(result.state, QStringLiteral("failed"));
        QCOMPARE(result.error, QStringLiteral("quota exceeded"));
    }

    void hunyuanBusinessErrorSurfacesMessage()
    {
        MockCloud3dServer server;
        QVERIFY(server.listen());
        server.enqueue("POST", "/", 200,
                       json({{"Response",
                              QJsonObject{{"Error",
                                           QJsonObject{{"Code", "AuthFailure"},
                                                       {"Message", "invalid secret id"}}}}}}));

        HunyuanProvider provider("AKIDEXAMPLE", "SECRETKEY", "ap-guangzhou", QString(),
                                 server.baseUrl());
        QSignalSpy failed(&provider, &HunyuanProvider::failed);
        provider.submit("a wooden crate", QString());

        QTRY_COMPARE(failed.count(), 1);
        const QString reason = failed.first().at(0).toString();
        QVERIFY(reason.contains(QStringLiteral("invalid secret id")));
        QVERIFY(!reason.startsWith(QStringLiteral("retryable:")));
    }
};

QTEST_MAIN(Cloud3dTest)
#include "tst_cloud3d.moc"
