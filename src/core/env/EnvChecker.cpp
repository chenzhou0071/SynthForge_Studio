#include "core/env/EnvChecker.h"

#include "core/mcp/McpRuntime.h"

#include <QProcess>
#include <QTimer>

#include <memory>

namespace {

constexpr int kCommandTimeoutMs = 5000; // uvx 卡住也不能把自检挂死

QJsonObject bridgeState(bool ready, int toolCount, const QString& notReadyReason)
{
    if (ready)
        return QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")},
                           {QStringLiteral("detail"),
                            QStringLiteral("已就绪 · %1 个工具").arg(toolCount)}};
    return QJsonObject{{QStringLiteral("state"), QStringLiteral("failed")},
                       {QStringLiteral("detail"), notReadyReason}};
}

} // namespace

EnvChecker::EnvChecker(QString uvxPath, McpRuntime* blender, McpRuntime* unity, QObject* parent)
    : QObject(parent), m_uvxPath(std::move(uvxPath)), m_blender(blender), m_unity(unity)
{
}

EnvChecker::~EnvChecker()
{
    // 同 StdioTransport 的教训：子进程还活着就销毁 QProcess，会触发强杀与告警
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
}

void EnvChecker::run()
{
    // 桥状态现在就取快照：QProcess 回调时 runtime 可能已经先被销毁了
    const QJsonObject blender = bridgeState(
        m_blender && m_blender->isReady(), m_blender ? m_blender->tools().size() : 0,
        QStringLiteral("blender-not-ready"));
    const QJsonObject unity = bridgeState(
        m_unity && m_unity->isReady(), m_unity ? m_unity->tools().size() : 0,
        QStringLiteral("unity-not-ready"));

    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    // 上一发自检还没回来就杀掉重跑：uvx 卡住时用户反复点[重新自检]不能攒一堆进程
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
    m_proc = proc;
    auto done = std::make_shared<bool>(false);
    QTimer::singleShot(kCommandTimeoutMs, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });

    const auto finish = [this, proc, blender, unity, done](int code) {
        if (*done)
            return; // finished 与 errorOccurred 只取先到者
        *done = true;
        const QString detail = QString::fromUtf8(proc->readAll()).trimmed();
        QJsonObject report;
        report.insert(QStringLiteral("uvx"),
                      code == 0 && !detail.isEmpty()
                          ? QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")},
                                         {QStringLiteral("detail"), detail}}
                          : QJsonObject{{QStringLiteral("state"), QStringLiteral("failed")},
                                         {QStringLiteral("detail"),
                                          detail.isEmpty() ? QStringLiteral("uvx-unavailable")
                                                           : detail}});
        report.insert(QStringLiteral("blender"), blender);
        report.insert(QStringLiteral("unity"), unity);
        // 程序不存在时 errorOccurred 会在 start() 里同步发射；改成排队发出，
        // 免得调用栈里就广播事件，也让结果到达时机保持一致。
        QMetaObject::invokeMethod(
            this, [this, report] { emit result(report); }, Qt::QueuedConnection);
        if (m_proc == proc)
            m_proc = nullptr; // 已经跳完一轮，析构时无需再杀
        proc->deleteLater();
    };

    connect(proc, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError) { finish(-1); }); // 可执行文件不存在也出结果
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus) { finish(code); });

    proc->start(m_uvxPath, {QStringLiteral("--version")});
}
