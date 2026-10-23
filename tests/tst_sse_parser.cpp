#include <QtTest>

#include "core/llm/SseParser.h"

class SseParserTest : public QObject
{
    Q_OBJECT

private slots:
    void splitsAcrossChunkBoundaries()
    {
        SseParser parser;
        QVERIFY(parser.feed("data: {\"a\":1}").isEmpty()); // 半帧不出
        const auto events = parser.feed("\n\ndata: [DONE]\n\n");
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.first().data, QByteArray("{\"a\":1}"));
        QCOMPARE(events.last().data, QByteArray("[DONE]"));
    }

    void handlesCrLfAndMultilineData()
    {
        SseParser parser;
        const auto events = parser.feed("data: line1\r\ndata: line2\r\n\r\n");
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("line1\nline2"));
    }
};

QTEST_MAIN(SseParserTest)
#include "tst_sse_parser.moc"
