#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class McpRuntime;
class QProcess;

// 启动环境自检：uvx 能不能跑 + 两条 MCP 链路是否就绪。
// 全程异步（QProcess + 5s 超时），一次 run() 发一次 result，不阻塞启动也不重复发。
class EnvChecker : public QObject
{
    Q_OBJECT

public:
    EnvChecker(QString uvxPath, McpRuntime* blender, McpRuntime* unity, QObject* parent = nullptr);
    ~EnvChecker() override; // 自检进程还在跑时必须先杀，否则 QProcess 析构会强杀子进程并报警告

    void run();

signals:
    void result(const QJsonObject& report);

private:
    QString m_uvxPath;
    McpRuntime* m_blender;
    McpRuntime* m_unity;
    QProcess* m_proc = nullptr; // 当前自检进程：重入时先杀掉旧的，不攒 uvx 进程
};
