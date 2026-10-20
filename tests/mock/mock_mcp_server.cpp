#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// 测试用 MCP stdio mock server：initialize / tools/list / tools/call(echo)。
// 每个响应故意拆成两次 write（中间 sleep），迫使客户端做跨 chunk 行缓冲才能解析。
namespace {

void writeSplit(const QJsonObject& response)
{
    const QByteArray out = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    const int half = static_cast<int>(out.size()) / 2;
    std::cout.write(out.constData(), half);
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout.write(out.constData() + half, out.size() - half);
    std::cout.flush();
}

} // namespace

int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back(); // cmake/管道在 Windows 下可能留下 CR
        if (line.empty())
            continue;

        const QJsonObject request =
            QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        const bool isNotification = !request.contains(QStringLiteral("id"));

        QJsonObject response{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), request.value(QStringLiteral("id"))}};

        if (method == QStringLiteral("ping")) {
            if (qEnvironmentVariableIsSet("MOCK_IGNORE_PING"))
                continue; // 故障注入：收到 ping 却不回复，模拟桥接卡死
            // 慢测试靠这行日志数心跳次数（走 stderr，不进协议通道）
            std::cerr << "mock: ping" << '\n' << std::flush;
            response.insert(QStringLiteral("result"), QJsonObject{});
        } else if (method == QStringLiteral("initialize")) {
            response.insert(
                QStringLiteral("result"),
                QJsonObject{
                    {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
                    {QStringLiteral("capabilities"),
                     QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
                    {QStringLiteral("serverInfo"),
                     QJsonObject{{QStringLiteral("name"), QStringLiteral("mock")},
                                 {QStringLiteral("version"), QStringLiteral("0.0.1")}}}});
        } else if (method == QStringLiteral("tools/list")) {
            const QJsonObject inputSchema{
                {"type", "object"},
                {"properties", QJsonObject{{"text", QJsonObject{{"type", "string"}}}}}};
            const QJsonObject echoTool{
                {QStringLiteral("name"), QStringLiteral("echo")},
                {QStringLiteral("description"), QStringLiteral("echo back the text argument")},
                {QStringLiteral("inputSchema"), inputSchema}};
            response.insert(QStringLiteral("result"),
                            QJsonObject{{QStringLiteral("tools"), QJsonArray{echoTool}}});
        } else if (method == QStringLiteral("tools/call")) {
            const QJsonObject arguments = request.value(QStringLiteral("params"))
                                              .toObject()
                                              .value(QStringLiteral("arguments"))
                                              .toObject();
            response.insert(
                QStringLiteral("result"),
                QJsonObject{{"content",
                             QJsonArray{QJsonObject{
                                 {"type", "text"},
                                 {"text", arguments.value(QStringLiteral("text")).toString()}}}}});
        } else {
            if (isNotification)
                continue;
            response.insert(QStringLiteral("error"),
                            QJsonObject{{"code", -32601}, {"message", "method not found"}});
        }

        if (isNotification)
            continue; // notifications/initialized 等不回复
        writeSplit(response);
    }
    return 0;
}
