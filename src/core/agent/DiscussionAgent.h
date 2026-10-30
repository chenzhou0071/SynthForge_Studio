#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

#include "core/llm/LlmGateway.h"

class LlmGateway;
class McpClient;

// 讨论模式的只读感知边界：三个自由函数把「能给模型看什么、能读什么」收在确定性代码里。
// execute_blender_code / import_asset / generate_3d 一律不进白名单——
// 「改」仍然只有确认草稿那一条路（规格 5.2 / 8.4 的边界）。
QJsonArray discussionTools(const QJsonArray& mcpTools);
bool isSafeObjectName(const QString& name);
QString inspectObjectScript(const QString& name);

// 讨论模式的只读感知：模型能问「场景里有什么、这个对象什么参数」，但不能改任何东西。
class DiscussionAgent : public QObject
{
    Q_OBJECT
public:
    DiscussionAgent(LlmGateway* llm, McpClient* mcp, QObject* parent = nullptr);

    void setModelConfig(const LlmConfig& cfg) { m_cfg = cfg; }
    // MCP tools/list 的原始结果由 CoreApp 注入：McpClient 自己不留副本
    void setMcpTools(const QJsonArray& tools) { m_mcpTools = tools; }
    // withTools=false（未连接 / Blender 忙 / 还没载入资产）时不发工具，退回纯文字讨论。
    // 就绪与否由调用方判断并体现在 withTools 上，这里不再二次 gate（否则单测无法覆盖带工具路径）
    void ask(const QString& area, const QString& systemPrompt, const QJsonArray& history,
             const QString& userText, bool withTools);

signals:
    void delta(const QString& area, const QString& text); // 已过 [[EXEC]] 过滤
    void toolStep(const QString& area, const QString& tool, const QString& status);
    void imageReceived(const QString& area, const QString& dataUrl, const QString& tool);
    void finished(const QString& area, const QString& visible, const QString& executable);
    void failed(const QString& area, const QString& reason);

private:
    void streamOnce(); // 发一轮对话：流式过滤 + 工具回调 + 无工具时收尾
    void seedSceneInfoThen(); // 先替模型跑一次 get_scene_info，把真实物体清单塞进上下文再开聊
    void executeCalls(const QJsonArray& calls);
    void finishRound();

    LlmGateway* m_llm;
    McpClient* m_mcp;
    LlmConfig m_cfg;
    QJsonArray m_mcpTools;
    QJsonArray m_messages;
    QJsonArray m_tools;
    QString m_area;
    QString m_stream;
    QString m_visible;
    int m_toolCalls = 0;
    int m_rounds = 0; // 查看「轮次」：一轮可并行看多个对象，按轮封顶而不是按单个调用
    int m_inFlight = 0;      // 本轮在途工具调用数：归零才继续下一轮对话
    bool m_roundDone = false; // 防封顶收尾与 onDone 双发 finished
    bool m_needSummary = false;    // 工具预算用完：本轮结束后强制一次「无工具直接总结」
    bool m_summaryAttempted = false; // 已强制总结过：绝不再循环第二轮
};
