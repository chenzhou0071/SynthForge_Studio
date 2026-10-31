#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

// 当前激活工程的对外描述：state ∈ inactive|deactivating|activating|active
struct ProjectInfo
{
    QString path;
    QString name;
    QString state;
    int attempt = 0;
};
Q_DECLARE_METATYPE(ProjectInfo)

// Unity 区唯一激活工程的状态机：校验目录、断旧链→取消旧任务→启新链的互斥切换、
// 等待 Unity 就绪（5s×12）。不感知 Blender 区任务（那由 CoreApp 的 BlenderGate 协调）。
class ProjectService : public QObject
{
    Q_OBJECT
public:
    enum class SwitchResult { Started, NeedsConfirm, Invalid, Same };

    explicit ProjectService(QObject* parent = nullptr);

    bool validate(const QString& path) const;
    // 校验 → 同路径 Same → 旧工程有运行/排队任务 NeedsConfirm（不切换）→ 否则 Started
    SwitchResult requestSwitch(const QString& path, int* runningTasks = nullptr);
    void confirmSwitch(const QString& path);
    void select(const QString& path);

    QString activePath() const { return m_active; }
    QString assetsPath() const;
    QString absoluteFor(const QString& relative) const;
    bool isInsideAssets(const QString& absolute) const;

    // 桥接生命周期由外层注入：Task 3 会把 stop/start 换成真实 m_unity 的 stop/start
    void setBridgeHooks(std::function<void()> stopBridge,
                        std::function<void()> startBridge,
                        std::function<void(const QString&)> cancelProjectTasks);
    void setBridgeReadyQuery(std::function<bool()> isReady);
    void setRunningTaskQuery(std::function<int()> runningTasks);
    void setWaitIntervalForTest(int ms);

signals:
    void stateChanged(const ProjectInfo& info);
    void waitingUnity(const QString& path, int attempt);
    void errorOccurred(const QString& reason);

private:
    void tryActivate();

    struct Hooks
    {
        std::function<void()> stopBridge;
        std::function<void()> startBridge;
        std::function<void(const QString&)> cancelProjectTasks;
        std::function<bool()> isReady;
        std::function<int()> runningTasks;
    };

    Hooks m_hooks;
    QString m_active;
    QTimer m_waitTimer;
    int m_waitAttempt = 0;
    int m_maxAttempts = 12;    // 5000ms × 12 = 60s（规格 6.2）
    int m_waitIntervalMs = 5000;
};
