#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>
#include <utility>

#include "core/agent/AgentOrchestrator.h"
#include "core/assets/AssetLibrary.h"
#include "core/llm/LlmGateway.h"
#include "core/mcp/BlenderGate.h"
#include "core/mcp/BridgeCommands.h" // BridgeCommand

class SettingsStore;
class SessionStore;
class TaskManager;
class ProtocolRouter;
class WsServer;
class McpRuntime;
class PreflightChecker;
class SummarizerAgent;
class Cloud3dService;
class UiState;
class DiscussionAgent;
class ProjectService;
class DirectoryWatcher;

struct CoreOptions
{
    QString dataDir;
    bool headless = false;
    QString libraryRoot;         // 空 = Documents/SynthForgeStudio/ModelLibrary；测试注入用
    BridgeCommand blenderBridge; // program 为空 = 默认 uvx mcp-for-blender；测试注入 mock_mcp_server
    BridgeCommand unityBridge;   // program 为空 = 默认 uvx mcp-for-unity；测试注入 mock/假程序
};

// 已确认、等用户选完存放目录就执行的任务
struct PendingPick
{
    QString area;
    QString mode;   // lowpoly | modify | cloud3d
    QString prompt; // 草稿卡编辑后的提示词
    BuildPlan plan;
};

class CoreApp : public QObject
{
    Q_OBJECT

public:
    explicit CoreApp(const CoreOptions& options, QObject* parent = nullptr);
    ~CoreApp() override;

    bool start();
    WsServer* ws() const { return m_ws.get(); }
    // 窗口几何与界面偏好共用同一实例，避免同进程里两个 QSettings 写同一个 ui.ini
    UiState* uiState() const { return m_uiState.get(); }

    using RestartFn = std::function<bool()>;
    // 由 P4（blender）/ P6（unity）注入桥接重启回调
    void registerBridge(const QString& area, RestartFn fn);

    // 原生目录选择：core 不依赖 QtWidgets，由 GUI 入口（main.cpp）注入 QFileDialog 回调。
    // 未注入（headless/单测）时 dialog.pickFolder 返回 dialog-unavailable。
    using FolderPicker =
        std::function<void(const QString& startDir, std::function<void(const QString&)> done)>;
    void setFolderPicker(FolderPicker picker) { m_folderPicker = std::move(picker); }

private:
    QJsonValue handleChatSend(const QJsonObject& params, bool appendUser = true);
    // 修改已有资产：单组计划直接执行，调用前草稿已转正为 user 行
    QJsonValue startModify(const QString& area, const QString& asset, const QString& text);
    // 按 .blend 相对路径回查该资产的产出形态（prop/diorama/open），查不到返回空
    QString assetShapeForPath(const QString& area, const QString& blendRelative) const;
    void broadcastConnStatus(const QString& area, const QString& status, const QString& hint);
    // Unity 编排器的信号接线：复用与 blender 同构的通用广播（不含 blender 专属的 plan/导出卡）
    void wireUnityOrchestrator(AgentOrchestrator* orch);
    // 新客户端连上时补发当前状态：桥接先就绪、页面后加载也不会永远灰灯
    void broadcastStatusSnapshot() const;
    LlmConfig primaryAgentConfig() const;
    QJsonArray recentMessages(const QString& area, int limit) const;
    // 规划/执行会把本次指令单独拼在末尾，历史里去掉同一条避免连续重复
    QJsonArray historyForPrompt(const QString& area, const QString& prompt, int limit) const;
    void openFolderPick(const QString& area);

    CoreOptions m_options;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<UiState> m_uiState;
    std::unique_ptr<SessionStore> m_sessions;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<ProtocolRouter> m_router;
    std::unique_ptr<WsServer> m_ws;
    std::unique_ptr<LlmGateway> m_llm;
    McpRuntime* m_blender = nullptr; // QObject child，析构由 parent 负责
    McpRuntime* m_unity = nullptr;   // QObject child：Unity 区 MCP 桥接运行时
    std::unique_ptr<PreflightChecker> m_preflight;
    SummarizerAgent* m_summarizer = nullptr; // QObject child，析构由 parent 负责
    std::unique_ptr<DiscussionAgent> m_ask;  // 讨论模式：只读感知 Blender 的多轮问答
    ProjectService* m_projects = nullptr;    // QObject child：Unity 区唯一激活工程状态机
    DirectoryWatcher* m_watcher = nullptr;   // QObject child：<project>/Assets 目录监控
    FolderPicker m_folderPicker;             // GUI 注入的文件夹选择回调（headless 为空）
    bool m_pickingFolder = false;            // 选择框打开中：并发请求一律 busy
    std::unique_ptr<Cloud3dService> m_cloud;  // 云 3D 轮询与下载，独立于任务队列
    AgentOrchestrator* m_orch = nullptr;     // QObject child，析构由 parent 负责
    AgentOrchestrator* m_unityOrch = nullptr; // Unity 区专用编排器：不能与 blender 共用，否则并行任务互相覆盖
    BlenderGate m_blenderGate; // Blender agent 任务与 unity 转换互斥
    QJsonArray m_blenderTools;
    QJsonArray m_unityTools;    // filterUnityTools 后的 Unity 区工具（白名单）
    std::unique_ptr<AssetLibrary> m_library;    // 资产库根目录（ModelLibrary）
    QHash<QString, QJsonObject> m_pendingPlans; // area -> 计划卡 JSON
    QHash<QString, OutputShape> m_shapeByArea;  // area -> 本次执行任务的档位（资产卡据此落 shape）
    QHash<QString, QString> m_planPrompts;      // area -> 已确认提示词
    QHash<QString, QString> m_pendingDraftAsset;  // area -> 本次草稿针对的资产
    QHash<QString, QString> m_pendingDraftSource; // area -> 用户原始需求（供「重新生成」）
    QHash<QString, QString> m_pendingDraftMode;   // area -> 本次草稿的模式（三种模板）
    QHash<QString, QString> m_pendingDraftShape;  // area -> 本次草稿的产出形态档位
    QHash<QString, QString> m_assetPrompts;       // area -> 本次执行的提示词（资产卡摘要）
    QHash<QString, qint64> m_regenDraft;          // area -> 正在重新生成的草稿 id
    QSet<QString> m_planCancelled;              // area -> 规划取消标记
    QHash<QString, PendingPick> m_pendingPicks; // area -> 等待选目录的执行任务
    QHash<QString, QString> m_pickRequests;     // requestId -> area
    // 讨论载入资产：requestId -> 区；区 -> 待载入的 .blend 绝对路径
    QHash<QString, QString> m_sceneLoadRequests;
    QHash<QString, QString> m_sceneLoadTargets;
    QHash<QString, QString> m_loadedScenes; // 区 -> 已载入的 .blend，Task 8 据此给只读工具
    // 讨论在途：area -> 任务 id / 目标资产 / 档位 / 用户原始需求（草稿行据此复原）
    QHash<QString, QString> m_askWait;
    QHash<QString, QString> m_askAsset;
    QHash<QString, QString> m_askShape;
    QHash<QString, QString> m_askSource;
    QHash<QString, QString> m_status; // area -> off|reconnecting|ready
    QHash<QString, QString> m_hints;  // area -> 状态提示文案
    QSet<QString> m_everReady;        // 曾握手成功的区：之后的 off 才算桥接崩溃
    QSet<QString> m_crashNotified;    // 本轮断链已出过卡，恢复 ready 时清除
    QHash<QString, QJsonObject> m_lastSend;     // area -> 最近一次 chat.send 参数
    QHash<QString, RestartFn> m_bridgeRestarts; // area -> 桥接重启回调
    qint64 m_seq = 0;
};
