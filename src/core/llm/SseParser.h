#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

class SseParser
{
public:
    struct Event
    {
        QString event;
        QByteArray data;
    };

    // 喂入任意大小的网络分片，只返回已完整的 SSE 帧；半帧留在缓冲区
    QList<Event> feed(const QByteArray& chunk);

private:
    QByteArray m_buffer;
};
