#pragma once

#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include "core/mcp/McpTransport.h"

struct BridgeCommand
{
    QString program;
    QStringList args;
    QProcessEnvironment env;
};

// 基于 QProcess 的 stdio 传输：拉起桥接子进程，按行切分 stdout，stderr 原样转发。
class StdioTransport : public McpTransport
{
    Q_OBJECT

public:
    explicit StdioTransport(const BridgeCommand& command, QObject* parent = nullptr);
    ~StdioTransport() override; // 析构时进程还活着则同步杀掉，避免 QProcess 警告后强杀

    void start() override;
    void sendLine(const QByteArray& line) override;
    void stop() override; // 异步：terminate 后 3 秒仍未退出才 kill，不阻塞主线程

private:
    void onReadyReadStdout();
    void onReadyReadStderr();

    BridgeCommand m_command;
    QProcess m_process;
    QByteArray m_buffer; // 跨 chunk 的行缓冲：readyRead 可能只拿到半行
};
