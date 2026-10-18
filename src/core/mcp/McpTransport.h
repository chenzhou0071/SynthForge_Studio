#pragma once

#include <QByteArray>
#include <QObject>

// 抽象传输接口：把一行 JSON 送出去，把收到的一行 JSON 抛上来。
// 契约：lineReceived 携带的行内容不含换行；sendLine 的入参已含尾部换行。
class McpTransport : public QObject
{
    Q_OBJECT

public:
    explicit McpTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~McpTransport() override = default;

    virtual void start() = 0;
    virtual void sendLine(const QByteArray& line) = 0;
    virtual void stop() = 0;

signals:
    void started();
    void lineReceived(const QByteArray& line);
    void stderrChunk(const QByteArray& chunk);
    void stopped(int exitCode);
};
