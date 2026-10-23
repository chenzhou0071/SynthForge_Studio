#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

#include "core/coreapp/CoreApp.h"
#include "core/mcp/BridgeCommands.h"
#include "core/mcp/McpClient.h"
#include "core/mcp/StdioTransport.h"
#include "core/settings/SettingsStore.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // 不设置 organizationName：Qt 会把 org 拼进 AppDataLocation，
    // 规格要求数据目录固定为 %APPDATA%/SynthForgeStudio
    QCoreApplication::setApplicationName(QStringLiteral("SynthForgeStudio"));

    // headless 核心服务：起 WebSocket，向 stderr 输出带 token 的地址后进入事件循环
    if (QCoreApplication::arguments().contains(QStringLiteral("--headless"))) {
        CoreApp core({defaultDataDir(), true});
        if (!core.start()) {
            std::fprintf(stderr, "error: core start failed (data dir: %s)\n",
                         defaultDataDir().toUtf8().constData());
            std::fflush(stderr);
            return 1;
        }
        return QCoreApplication::exec();
    }

    QCoreApplication::setApplicationName(QStringLiteral("synthforge-cli"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("SynthForge MCP 冒烟 CLI：连接 Blender / Unity 桥接并列工具或调用工具"));
    parser.addHelpOption();
    parser.addOption({{QStringLiteral("c"), QStringLiteral("connect")},
                      QStringLiteral("连接目标：blender 或 unity"), QStringLiteral("area")});
    parser.addOption({QStringLiteral("list-tools"), QStringLiteral("列出服务端全部工具后退出")});
    parser.addOption({QStringLiteral("call"), QStringLiteral("调用指定工具并打印结果"),
                      QStringLiteral("tool")});
    parser.addOption({QStringLiteral("args"),
                      QStringLiteral("工具参数 JSON 对象"), QStringLiteral("json"),
                      QStringLiteral("{}")});
    parser.addOption({QStringLiteral("timeout"),
                      QStringLiteral("单请求超时毫秒"), QStringLiteral("ms"),
                      QStringLiteral("30000")});
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const BridgeCommand command = defaultBridgeCommand(parser.value(QStringLiteral("connect")));
    if (command.program.isEmpty()) {
        err << "error: --connect 必须为 blender 或 unity\n";
        err.flush();
        return 2;
    }

    // 参数在连接前校验完，避免连上桥接后才因为 JSON 写错而白跑一次握手
    QJsonObject arguments;
    if (parser.isSet(QStringLiteral("call"))) {
        QJsonParseError parseError;
        const QJsonDocument argsDoc =
            QJsonDocument::fromJson(parser.value(QStringLiteral("args")).toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !argsDoc.isObject()) {
            err << "error: --args 必须是 JSON 对象：" << parseError.errorString() << '\n';
            err.flush();
            return 2;
        }
        arguments = argsDoc.object();
    }

    bool timeoutOk = false;
    const int timeoutMs = parser.value(QStringLiteral("timeout")).toInt(&timeoutOk);
    if (!timeoutOk || timeoutMs <= 0) {
        err << "error: --timeout 必须是正整数毫秒\n";
        err.flush();
        return 2;
    }

    auto* transport = new StdioTransport(command, &app);
    auto* client = new McpClient(transport, &app);
    client->setRequestTimeout(timeoutMs);

    // 退出前先 shutdown（关心跳 + 停 transport，且不触发自动重连）。
    // stdio 服务端收到 stdin EOF 后自行退出，否则 app 析构时 QProcess 还活着，
    // Qt 会强杀子进程并打印 Destroyed 警告。
    bool finishing = false;
    int finishCode = 0;
    // QProcess 的 FailedToStart 在 start() 内部同步发射，此时事件循环还没运行，
    // 直接 exit() 会被 Qt 忽略导致挂死；统一推迟到下一轮事件循环再退。
    const auto quitNow = [&](int code) {
        QTimer::singleShot(0, &app, [&app, code] { QCoreApplication::exit(code); });
    };
    const auto finish = [&](int code) {
        if (finishing)
            return;
        finishing = true;
        finishCode = code;
        client->shutdown();
        QTimer::singleShot(4000, &app, [&] { quitNow(finishCode); });
    };

    QObject::connect(client, &McpClient::stderrReceived, &app, [](const QByteArray& chunk) {
        // 按原字节透传：Python 桥接日志在 Windows 管道里是 GBK，
        // 先 fromUtf8 再写控制台会把中文变成乱码
        fwrite(chunk.constData(), 1, static_cast<size_t>(chunk.size()), stderr);
        fflush(stderr);
    });
    QObject::connect(client, &McpClient::failed, &app, [&](const QString& reason) {
        err << "error: " << reason << '\n';
        err.flush();
        finish(1);
    });
    QObject::connect(client, &McpClient::ready, &app, [&](const QJsonObject& serverInfo) {
        err << "connected: " << serverInfo.value(QStringLiteral("name")).toString() << ' '
            << serverInfo.value(QStringLiteral("version")).toString() << '\n';
        err.flush();

        if (parser.isSet(QStringLiteral("call"))) {
            client->callTool(parser.value(QStringLiteral("call")), arguments,
                             [&](const QJsonValue& result, const QJsonObject& error) {
                                 if (!error.isEmpty()) {
                                     err << "tool error: "
                                         << QJsonDocument(error).toJson(QJsonDocument::Compact)
                                         << '\n';
                                     err.flush();
                                     finish(1);
                                     return;
                                 }
                                 if (result.isObject()) {
                                     out << QJsonDocument(result.toObject())
                                                .toJson(QJsonDocument::Indented);
                                 } else if (result.isArray()) {
                                     out << QJsonDocument(result.toArray())
                                                .toJson(QJsonDocument::Indented);
                                 } else {
                                     out << "\"" << result.toString() << "\"\n";
                                 }
                                 out.flush();
                                 finish(0);
                             });
        } else {
            client->listTools([&](const QJsonArray& tools) {
                out << QJsonDocument(tools).toJson(QJsonDocument::Indented);
                out.flush();
                finish(0);
            });
        }
    });
    QObject::connect(transport, &McpTransport::stopped, &app, [&](int exitCode) {
        if (finishing) {
            quitNow(finishCode); // 主动退出：用调用方指定的退出码
            return;
        }
        err << "bridge exited with code " << exitCode << '\n';
        err.flush();
        quitNow(1);
    });
    client->start(); // 启动传输；started 后由 McpClient 自动握手
    return app.exec();
}
