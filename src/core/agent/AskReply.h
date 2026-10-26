#pragma once

#include <QString>

// 讨论模式的回答分两部分：正文给用户看，[[EXEC]] 块是可直接执行的草稿。
struct AskReply
{
    QString visible;
    QString executable;
};

AskReply parseAskReply(const QString& raw);

// 流式过滤：[[EXEC]] 及之后的草稿留在服务端，不发给前端。
// 标记可能被 delta 边界切开，所以先压住最多一个标记长度再吐。
class AskStreamFilter
{
public:
    QString feed(const QString& delta); // 返回本次可新增展示的文本
    QString finish();                   // 结束：补发尚未吐出的可见尾巴

private:
    QString m_pending;
    bool m_hidden = false;
};
