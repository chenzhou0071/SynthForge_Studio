#pragma once

#include "core/llm/LlmGateway.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>

class QTcpSocket;

namespace MockLlm {
// 构造 OpenAI 流式 wire format 的 SSE 帧（只造字节，不参与解析）
QByteArray textChunk(const QString& text); // choices[0].delta.content
QByteArray toolCallStartChunk(int index, const QString& id, const QString& name,
                              const QString& argsFragment);
QByteArray toolArgsChunk(int index, const QString& argsFragment); // 只带 index + arguments 续片
QByteArray toolCallChunk(const QString& name, const QString& args = QStringLiteral("{}"));
QByteArray doneChunk(); // "data: [DONE]\n\n"
} // namespace MockLlm

// 假 LLM 服务端：真实 TCP + chunked SSE，专给 LlmGateway 的网络/聚合代码做测试。
// 不是 LlmGateway 子类——测试必须让生产代码真发 POST /chat/completions。
class MockLlmServer : public QObject
{
    Q_OBJECT

public:
    struct Turn
    {
        QList<QByteArray> frames; // 每个元素是一整帧（可带也可不带结尾空行）
        int splitEvery = 3;       // 每帧再切成 N 字节一片经 TCP 发送；0 = 整帧
        int frameDelayMs = 2;     // 片与片之间延迟，确保 readyRead 分多次到达
        int status = 200;         // != 200：回错误 JSON，不发 SSE
        QByteArray errorBody = R"({"error":{"message":"mock error"}})";
        bool stallAfterFrames = false; // 发完 frames 保持连接（abort 用例）
    };

    explicit MockLlmServer(QList<Turn> script, QObject* parent = nullptr);
    bool listen(); // 127.0.0.1，端口 0 由系统分配
    QString baseUrl() const;
    LlmConfig config(const QString& model = QStringLiteral("test-model")) const;
    int requestCount() const { return int(m_requests.size()); }
    QString pathAt(int index) const { return m_paths.value(index); }
    QByteArray authorizationAt(int index) const { return m_authorizations.value(index); }
    QString requestAt(int index) const { return m_requests.value(index); } // compact JSON 文本
    QJsonObject requestObjectAt(int index) const;

private:
    void onNewConnection();
    void respond(QTcpSocket* socket, const Turn& turn);
    Turn takeTurn();

    QTcpServer m_server;
    QList<Turn> m_script;
    int m_next = 0;
    QList<QString> m_requests; // compact JSON 文本
    QList<QString> m_paths;
    QList<QByteArray> m_authorizations;
};
