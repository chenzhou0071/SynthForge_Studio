#include "core/mcp/StdioTransport.h"

#include <QTimer>

StdioTransport::StdioTransport(const BridgeCommand& command, QObject* parent)
    : McpTransport(parent), m_command(command)
{
    connect(&m_process, &QProcess::started, this, &McpTransport::started);
    connect(&m_process, &QProcess::readyReadStandardOutput, this,
            &StdioTransport::onReadyReadStdout);
    connect(&m_process, &QProcess::readyReadStandardError, this,
            &StdioTransport::onReadyReadStderr);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // CrashExit 由 finished 上报；这里只处理进程压根没起来的情况，避免 stopped 发两次
        if (error != QProcess::FailedToStart)
            return;
        emit stderrChunk(m_process.errorString().toUtf8() + '\n');
        emit stopped(-1);
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { emit stopped(exitCode); });
}

StdioTransport::~StdioTransport()
{
    // 析构时进程还活着，说明调用方没走 shutdown/stop 就销毁了传输。
    // 同步杀掉并短等待，否则 QProcess 析构会打印 Destroyed 警告后强杀。
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void StdioTransport::start()
{
    if (m_process.state() != QProcess::NotRunning)
        return; // 幂等：重复 start 不重启已在运行的进程
    m_buffer.clear(); // 重连时丢弃上次连接的残留半行
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_command.env.isEmpty())
        m_process.setProcessEnvironment(m_command.env);
    m_process.start(m_command.program, m_command.args);
}

void StdioTransport::sendLine(const QByteArray& line)
{
    m_process.write(line); // line 已含尾部 '\n'
}

void StdioTransport::stop()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    // stdio 服务端的标准退出信号是 stdin EOF：先关写通道让它自行收尾。
    // Windows 下 terminate（WM_CLOSE）对没有消息循环的 console 子进程无效，
    // 所以 3 秒兜底强杀；全程不在主线程 waitForFinished，避免冻结 UI。
    const qint64 pid = m_process.processId();
    m_process.closeWriteChannel();
    QTimer::singleShot(3000, this, [this, pid] {
        // pid 校验：3 秒内若已重连出新进程，不能误杀
        if (m_process.state() != QProcess::NotRunning && m_process.processId() == pid)
            m_process.kill();
    });
}

void StdioTransport::onReadyReadStdout()
{
    m_buffer += m_process.readAllStandardOutput();
    int newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (!line.isEmpty())
            emit lineReceived(line);
    }
}

void StdioTransport::onReadyReadStderr()
{
    emit stderrChunk(m_process.readAllStandardError());
}
