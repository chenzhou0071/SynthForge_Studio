#include "core/unity/ProjectService.h"

#include <QDir>
#include <QFileInfo>

#include <utility>

ProjectService::ProjectService(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ProjectInfo>();
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, &ProjectService::tryActivate);
}

bool ProjectService::validate(const QString& path) const
{
    const QDir dir(path);
    return dir.exists(QStringLiteral("Assets")) && dir.exists(QStringLiteral("ProjectSettings"));
}

ProjectService::SwitchResult ProjectService::requestSwitch(const QString& path, int* running)
{
    if (!validate(path))
        return SwitchResult::Invalid;
    if (path == m_active)
        return SwitchResult::Same;
    const int count = m_hooks.runningTasks ? m_hooks.runningTasks() : 0;
    if (running)
        *running = count;
    if (!m_active.isEmpty() && count > 0)
        return SwitchResult::NeedsConfirm;              // 前端弹确认框，不切换
    select(path);
    return SwitchResult::Started;
}

void ProjectService::confirmSwitch(const QString& path)
{
    select(path);
}

void ProjectService::select(const QString& path)
{
    if (!validate(path)) {
        emit errorOccurred(QStringLiteral("invalid-project"));
        return;
    }
    if (path == m_active)
        return;

    m_waitTimer.stop();
    m_waitAttempt = 0;

    // 切换顺序固定：旧工程 deactivating（取消任务 + 断 MCP）→ 新工程 activating → active
    if (!m_active.isEmpty()) {
        emit stateChanged({m_active, QFileInfo(m_active).fileName(),
                           QStringLiteral("deactivating"), 0});
        if (m_hooks.cancelProjectTasks)
            m_hooks.cancelProjectTasks(m_active);       // 终止该工程绑定的 Agent 任务
        if (m_hooks.stopBridge)
            m_hooks.stopBridge();                       // 断开旧 MCP 会话
    }

    emit stateChanged({path, QFileInfo(path).fileName(),
                       QStringLiteral("activating"), 0});
    m_active = path;
    if (m_hooks.startBridge)
        m_hooks.startBridge();                          // 拉起新 MCP 会话
    tryActivate();
}

void ProjectService::tryActivate()
{
    if (m_active.isEmpty())
        return;
    if (!m_hooks.isReady || m_hooks.isReady()) {        // 未注入视为已就绪（单测/无桥路径）
        m_waitTimer.stop();
        emit stateChanged({m_active, QFileInfo(m_active).fileName(),
                           QStringLiteral("active"), 0});
        return;
    }
    if (++m_waitAttempt > m_maxAttempts) {              // 5s × 12 = 60s
        emit errorOccurred(QStringLiteral("unity-not-ready"));
        return;                                         // 保持 activating，可再次 project.select
    }
    emit waitingUnity(m_active, m_waitAttempt);
    m_waitTimer.start(m_waitIntervalMs);
}

QString ProjectService::assetsPath() const
{
    return m_active.isEmpty() ? QString() : m_active + QStringLiteral("/Assets");
}

QString ProjectService::absoluteFor(const QString& relative) const
{
    return QDir(assetsPath()).filePath(relative);
}

bool ProjectService::isInsideAssets(const QString& absolute) const
{
    if (m_active.isEmpty())
        return false;
    const QString root = QFileInfo(assetsPath()).canonicalFilePath();
    QString path = QFileInfo(absolute).canonicalFilePath();
    if (path.isEmpty()) {                               // 目标尚不存在：校验父目录
        const QFileInfo info(absolute);
        path = QDir::cleanPath(info.absoluteDir().canonicalPath() + QStringLiteral("/") + info.fileName());
    }
    return !path.isEmpty() && (path == root || path.startsWith(root + QLatin1Char('/')));
}

void ProjectService::setBridgeHooks(std::function<void()> stopBridge,
                                    std::function<void()> startBridge,
                                    std::function<void(const QString&)> cancelProjectTasks)
{
    m_hooks.stopBridge = std::move(stopBridge);
    m_hooks.startBridge = std::move(startBridge);
    m_hooks.cancelProjectTasks = std::move(cancelProjectTasks);
}

void ProjectService::setBridgeReadyQuery(std::function<bool()> isReady)
{
    m_hooks.isReady = std::move(isReady);
}

void ProjectService::setRunningTaskQuery(std::function<int()> runningTasks)
{
    m_hooks.runningTasks = std::move(runningTasks);
}

void ProjectService::setWaitIntervalForTest(int ms)
{
    m_waitIntervalMs = ms;
}
