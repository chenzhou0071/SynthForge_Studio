#include <QtTest>

#include "core/agent/AskReply.h"

class AskReplyTest : public QObject
{
    Q_OBJECT
private slots:
    void plainAnswerHasNoExecutable()
    {
        const AskReply r = parseAskReply(QStringLiteral("建议把招牌换成旧铁皮。"));
        QCOMPARE(r.visible, QStringLiteral("建议把招牌换成旧铁皮。"));
        QVERIFY(r.executable.isEmpty());
    }

    void execBlockIsSplitOut()
    {
        const AskReply r = parseAskReply(QStringLiteral(
            "建议从材质和破损两处入手。\n\n[[EXEC]]\n把招牌换成深绿做旧铁皮\n[[/EXEC]]"));
        QCOMPARE(r.visible, QStringLiteral("建议从材质和破损两处入手。"));
        QCOMPARE(r.executable, QStringLiteral("把招牌换成深绿做旧铁皮"));
    }

    void unclosedBlockTakesRest()
    {
        const AskReply r = parseAskReply(QStringLiteral("方案如下。\n[[EXEC]]\n主题与视角：雨夜街角"));
        QCOMPARE(r.visible, QStringLiteral("方案如下。"));
        QCOMPARE(r.executable, QStringLiteral("主题与视角：雨夜街角"));
    }

    void emptyBlockIsNotExecutable()
    {
        const AskReply r = parseAskReply(QStringLiteral("先不定。\n[[EXEC]][[/EXEC]]"));
        QCOMPARE(r.visible, QStringLiteral("先不定。"));
        QVERIFY(r.executable.isEmpty());
    }

    // 模板里的占位说明会被模型原样抄进草稿，卡片上不能出现这行
    void stripsCopiedPlaceholderLine()
    {
        const AskReply r = parseAskReply(QStringLiteral(
            "方案如下。\n[[EXEC]]\n（可直接执行的草稿）\n\n主题与视角：小卧室\n[[/EXEC]]"));
        QCOMPARE(r.visible, QStringLiteral("方案如下。"));
        QCOMPARE(r.executable, QStringLiteral("主题与视角：小卧室"));
    }

    // 流式期间只发正文；[[EXEC]] 之后的内容必须留在服务端，等结束变成草稿卡
    void streamFilterHidesExecBlockAcrossChunks()
    {
        AskStreamFilter filter;
        const QString raw = QStringLiteral(
            "建议先改材质。\n\n[[EXEC]]\n把招牌换成深绿铁皮\n[[/EXEC]]");
        QString shown;
        for (int i = 0; i < raw.size(); i += 2)
            shown += filter.feed(raw.mid(i, 2));
        shown += filter.finish();

        QCOMPARE(shown, QStringLiteral("建议先改材质。\n\n"));
        QVERIFY(!shown.contains(QStringLiteral("[[EXEC]]")));
        QVERIFY(!shown.contains(QStringLiteral("深绿铁皮")));
    }

    void streamFilterFlushesPlainTail()
    {
        AskStreamFilter filter;
        QString shown = filter.feed(QStringLiteral("你好"));
        shown += filter.finish();
        QCOMPARE(shown, QStringLiteral("你好"));
    }
};

QTEST_MAIN(AskReplyTest)
#include "tst_ask_reply.moc"
