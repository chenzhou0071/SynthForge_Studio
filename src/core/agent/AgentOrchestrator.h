#pragma once

#include "core/llm/LlmGateway.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class TaskManager;
class McpClient;
class BlenderGate;

struct BuildPlacement
{
    QString name;
    QString where;
};

struct BuildGroup
{
    QString title;
    QStringList items;
    QVector<BuildPlacement> placements;   // 每个 item 一条：相对谁、朝哪、离地多少
    QString acceptance;
};

struct BuildPlan
{
    QVector<BuildGroup> groups;
    QStringList rules;                    // 本任务特有的跨组布局约束
    int maxSteps = 50;
};

// 两阶段主循环：plan() 只产计划（纯文本，不碰工具），execute() 按组「建造 → 自检」推进，
// 最后一组结束再走一个导出回合。工具调用一律经 McpClient，编排层不区分 MCP/云。
class AgentOrchestrator : public QObject
{
    Q_OBJECT
public:
    AgentOrchestrator(LlmGateway* llm, McpClient* mcp, QObject* parent = nullptr);

    void setModelConfig(const LlmConfig& cfg) { m_cfg = cfg; }
    void plan(const QString& taskId, TaskManager* tm, const QString& prompt, const QString& area);
    void execute(const QString& taskId, TaskManager* tm, const QJsonArray& tools,
                 const BuildPlan& plan, const QString& prompt, const QString& targetFolder,
                 const QString& area);
    void setBlenderGate(BlenderGate* gate) { m_gate = gate; }
    void setToolCallHook(std::function<void(const QString& name, bool running)> hook);
    void setConversationHistory(const QJsonArray& history);
    bool isPlanning(const QString& area) const { return m_planning.contains(area); }

signals:
    void planReady(const QString& area, const QString& taskId, const QJsonObject& plan);
    void groupProgress(const QString& area, int index, int count, const QString& title);
    void finalText(const QString& area, const QString& text);
    void failed(const QString& area, const QString& reason);
    void toolStep(const QString& area, const QString& tool, const QString& status);
    void delta(const QString& area, const QString& text);
    void imageReceived(const QString& area, const QString& taskId, const QString& dataUrl,
                       const QString& caption);
    void artifactsReady(const QString& area, const QStringList& absoluteGlbs);

private:
    enum class GroupPhase
    {
        Build,
        SelfCheck
    };

    QJsonArray initialMessages(const QString& system, const QString& user) const;
    void attemptPlan(int attempt);
    void startGroup();
    void tick();
    void onTurnFinished();
    void executeToolCalls(const QJsonArray& calls);
    void finishOk(const QString& text);
    void finishCancelled();
    void fail(const QString& reason);
    void releaseGate();
    QStringList collectNewGlbs() const;

    LlmGateway* m_llm;
    McpClient* m_mcp;
    TaskManager* m_tm = nullptr;
    LlmConfig m_cfg;
    QJsonArray m_tools; // OpenAI tools（CoreApp 经 ToolBridge 转换后传入）
    QJsonArray m_history;
    QJsonArray m_messages;
    BuildPlan m_plan;
    QString m_taskId;
    QString m_area;
    QString m_prompt;
    QString m_targetFolder;
    QString m_stream;
    int m_steps = 0;
    int m_maxSteps = 50;
    int m_groupIndex = 0;
    int m_pendingToolCalls = 0;
    bool m_awaitingTools = false;
    bool m_exporting = false; // 最后一组的导出回合已发起
    GroupPhase m_phase = GroupPhase::Build;
    QSet<QString> m_planning; // 规划进行中的区（chat.cancel 可丢弃迟到结果）
    QDateTime m_startedAt;
    BlenderGate* m_gate = nullptr; // 仅 blender orchestrator 注入
    bool m_gateHeld = false;
    std::function<void(const QString&, bool)> m_toolCallHook;
};
