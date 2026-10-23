#include "core/llm/SseParser.h"

QList<SseParser::Event> SseParser::feed(const QByteArray& chunk)
{
    m_buffer += chunk;
    m_buffer.replace("\r\n", "\n"); // 每次拼接后替换，兼容 CRLF 被拆到两个分片

    QList<Event> out;
    int frameEnd = -1;
    while ((frameEnd = m_buffer.indexOf("\n\n")) >= 0) {
        const QByteArray frame = m_buffer.left(frameEnd);
        m_buffer.remove(0, frameEnd + 2);

        Event event;
        for (const QByteArray& line : frame.split('\n')) {
            if (line.startsWith("event:"))
                event.event = QString::fromUtf8(line.mid(6).trimmed());
            else if (line.startsWith("data:")) {
                if (!event.data.isEmpty())
                    event.data += '\n'; // 多行 data 之间用换行拼接
                event.data += line.mid(5).trimmed();
            }
        }
        if (!event.data.isEmpty() || !event.event.isEmpty())
            out.append(event);
    }
    return out;
}
