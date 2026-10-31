#pragma once

#include "core/mcp/McpClient.h"
#include "core/mcp/McpTransport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

// 内存 MCP 替身：不启真实进程，把 tools/call 一律回成功（content 里一段 text）。
// 与 McpClient 组合使用而非继承，让编排层的 callTool 调用路径保持真实。
class MockMcpHarness : public McpTransport
{
public:
    void start() override { emit started(); }

    void sendLine(const QByteArray& line) override
    {
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        const int id = request.value(QStringLiteral("id")).toInt();

        if (method == QLatin1String("initialize")) {
            reply(id, QJsonObject{{"protocolVersion", "2025-06-18"},
                                  {"serverInfo", QJsonObject{{"name", "mock-harness"},
                                                             {"version", "0"}}},
                                  {"capabilities", QJsonObject{}}});
            return;
        }
        if (method == QLatin1String("tools/call")) {
            const QJsonObject params = request.value(QStringLiteral("params")).toObject();
            calls.append(params.value(QStringLiteral("name")).toString());
            if (transientFailures > 0) { // 模拟 domain reload 期间的编辑器掉线
                --transientFailures;
                reply(id, QJsonObject{{"isError", true},
                                      {"content", QJsonArray{QJsonObject{
                                           {"type", "text"},
                                           {"text", "未找到 Unity 编辑器实例"}}}}});
                return;
            }
            reply(id, QJsonObject{{"content", QJsonArray{QJsonObject{
                                               {"type", "text"}, {"text", "ok"}}}}});
            return;
        }
        reply(id, QJsonObject{{"tools", QJsonArray{}}});
    }

    void stop() override { emit stopped(0); }

    McpClient& client() { return m_client; }
    QStringList calls;        // 收到的工具名，按调用顺序
    int transientFailures = 0; // 前 N 次 tools/call 回「未找到 Unity 编辑器实例」

private:
    void reply(int id, const QJsonObject& result)
    {
        // 异步回包：sendLine 此刻还在 McpClient::sendRequest 栈上，直接 emit 会重入
        const QByteArray payload =
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
                .toJson(QJsonDocument::Compact);
        QTimer::singleShot(0, this, [this, payload]() { emit lineReceived(payload); });
    }

    McpClient m_client{this};
};
