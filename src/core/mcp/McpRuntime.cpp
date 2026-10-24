#include "core/mcp/McpRuntime.h"

#include "core/mcp/McpClient.h"
#include "core/mcp/McpTransport.h"

McpRuntime::McpRuntime(const QString& area, std::unique_ptr<McpTransport> transport,
                       QObject* parent)
    : QObject(parent)
    , m_area(area)
    , m_transport(std::move(transport))
    , m_client(new McpClient(m_transport.get(), this))
{
    connect(m_client, &McpClient::ready, this, [this] {
        refreshTools();
        emit statusChanged(m_area, QStringLiteral("ready"), QString());
    });
    connect(m_client, &McpClient::reconnected, this, [this] {
        refreshTools();
        emit statusChanged(m_area, QStringLiteral("ready"), QString());
    });
    connect(m_client, &McpClient::reconnecting, this, [this](int attempt) {
        emit statusChanged(m_area, QStringLiteral("reconnecting"),
                           QStringLiteral("第 %1 次重连").arg(attempt));
    });
    connect(m_client, &McpClient::failed, this, [this](const QString& reason) {
        emit statusChanged(m_area, QStringLiteral("off"), reason);
    });
}

McpRuntime::~McpRuntime()
{
    // 先断链清空 pending：transport 析构时 kill/waitForFinished 可能同步放出残留响应，
    // 此时 m_tools 已先于 m_transport 销毁，回调再写它就是访问已释放内存。
    m_client->shutdown();
}

bool McpRuntime::start()
{
    m_client->start(); // 内部启动传输，started 后自动握手
    return true;
}

void McpRuntime::stop()
{
    m_client->shutdown(); // 主动断开：不触发自动重连
    emit statusChanged(m_area, QStringLiteral("off"), QString());
}

bool McpRuntime::restart()
{
    m_client->shutdown();       // 主动断开：抑制旧会话的自动重连，不报 bridge-crashed
    m_client->prepareRestart(); // bridge.restart 语义：启动失败仍可退避重试（不被 shutdown 标志卡死）
    m_client->start();          // 重新启动传输；started → 自动重新握手
    return true;
}

bool McpRuntime::isReady() const
{
    return m_client->isReady();
}

void McpRuntime::refreshTools()
{
    m_client->listTools([this](const QJsonArray& tools) {
        m_tools = tools;
        emit toolsChanged(m_tools);
    });
}
