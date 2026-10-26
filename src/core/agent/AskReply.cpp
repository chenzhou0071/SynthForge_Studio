#include "core/agent/AskReply.h"

#include <QRegularExpression>

namespace {
const QString kOpen = QStringLiteral("[[EXEC]]");
const QString kClose = QStringLiteral("[[/EXEC]]");

// 模板里的占位说明经常被模型原样抄进草稿，卡片上不能出现这行
QString stripPlaceholder(QString text)
{
    text = text.trimmed();
    const int lineEnd = text.indexOf(QLatin1Char('\n'));
    const QString firstLine = (lineEnd < 0 ? text : text.left(lineEnd)).trimmed();
    static const QRegularExpression placeholder(
        QStringLiteral(R"(^[（(][^）)]*(?:草稿正文|可直接执行的草稿)[^）)]*[）)]$)"));
    if (firstLine.isEmpty() || !placeholder.match(firstLine).hasMatch())
        return text;
    return lineEnd < 0 ? QString() : text.mid(lineEnd + 1).trimmed();
}
} // namespace

AskReply parseAskReply(const QString& raw)
{
    AskReply reply;
    const int start = raw.indexOf(kOpen);
    if (start < 0) {
        reply.visible = raw.trimmed();
        return reply;
    }
    const int bodyStart = start + kOpen.size();
    const int end = raw.indexOf(kClose, bodyStart);
    // 模型偶尔忘写闭合标记：把剩余内容整段当成草稿，不让用户看到半截标记
    reply.executable = stripPlaceholder(
        end < 0 ? raw.mid(bodyStart) : raw.mid(bodyStart, end - bodyStart));
    reply.visible = raw.left(start).trimmed();
    return reply;
}

QString AskStreamFilter::feed(const QString& delta)
{
    if (m_hidden)
        return {};
    m_pending += delta;
    const int at = m_pending.indexOf(kOpen);
    if (at >= 0) {
        const QString visible = m_pending.left(at);
        m_pending.clear();
        m_hidden = true;
        return visible;
    }
    // 标记可能被切成多帧：吐出前先留一个标记长度，避免 [[EXE 先露出去
    const int hold = kOpen.size() - 1;
    if (m_pending.size() <= hold)
        return {};
    const QString visible = m_pending.left(m_pending.size() - hold);
    m_pending = m_pending.right(hold);
    return visible;
}

QString AskStreamFilter::finish()
{
    if (m_hidden)
        return {};
    const QString visible = m_pending;
    m_pending.clear();
    return visible;
}
