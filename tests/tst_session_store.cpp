#include <QFile>
#include <QJsonArray>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#include "core/store/SessionStore.h"

namespace {

void appendRange(SessionStore& store, const QString& area, int from, int to)
{
    for (int i = from; i <= to; ++i)
        store.appendMessage(area, QStringLiteral("user"), QStringLiteral("m%1").arg(i));
}

} // namespace

class SessionStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void messagesRoundTripPerArea()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        store.appendMessage(QStringLiteral("blender"), QStringLiteral("user"),
                            QStringLiteral("做一个便利店"));
        store.appendMessage(QStringLiteral("blender"), QStringLiteral("assistant"),
                            QStringLiteral("好的"));
        store.appendMessage(QStringLiteral("unity"), QStringLiteral("user"),
                            QStringLiteral("建一个地面"));

        const QJsonArray blenderMsgs = store.messages(QStringLiteral("blender"));
        QCOMPARE(blenderMsgs.size(), 2);
        QCOMPARE(blenderMsgs.first().toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("做一个便利店"));
        QCOMPARE(store.messages(QStringLiteral("unity")).size(), 1);
    }

    void draftUpsertKeepsLatest()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        store.saveDraft(QStringLiteral("blender"), QStringLiteral("第一版"));
        store.saveDraft(QStringLiteral("blender"), QStringLiteral("第二版"));
        QCOMPARE(store.draft(QStringLiteral("blender")), QStringLiteral("第二版"));
    }

    void walModePersistedInFileHeader()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.path() + QStringLiteral("/sessions.db");
        {
            SessionStore store(dbPath);
            QVERIFY(store.open());
            store.appendMessage(QStringLiteral("blender"), QStringLiteral("user"),
                                QStringLiteral("hello"));
        } // 析构关闭连接，确保文件头和 WAL 状态落盘

        QFile file(dbPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray header = file.read(100);
        QCOMPARE(header.size(), 100);
        // SQLite 文件头偏移 18/19：2 = WAL，1 = 传统回滚日志
        QCOMPARE(quint8(header.at(18)), quint8(2));
        QCOMPARE(quint8(header.at(19)), quint8(2));
    }

    void pageReturnsLatestPageAscending()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        appendRange(store, QStringLiteral("blender"), 1, 45);
        for (int i = 0; i < 5; ++i) // 穿插另一个区，验证分页不串区
            store.appendMessage(QStringLiteral("unity"), QStringLiteral("user"),
                                QStringLiteral("u%1").arg(i));

        const MessagePage page = store.messagePage(QStringLiteral("blender"), 20);
        QCOMPARE(page.messages.size(), 20);
        QCOMPARE(page.messages.first().toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("m26"));
        QCOMPARE(page.messages.last().toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("m45"));
        QVERIFY(page.hasMore);
        QCOMPARE(page.nextCursor,
                 page.messages.first().toObject().value(QStringLiteral("id")).toInteger());
        QVERIFY(page.messages.first().toObject().value(QStringLiteral("ts")).toInteger() > 0);
    }

    void pageWalksBackwardsWithoutGaps()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        appendRange(store, QStringLiteral("blender"), 1, 45);

        const MessagePage first = store.messagePage(QStringLiteral("blender"), 20);
        const MessagePage second =
            store.messagePage(QStringLiteral("blender"), 20, first.nextCursor);
        const MessagePage third =
            store.messagePage(QStringLiteral("blender"), 20, second.nextCursor);

        QCOMPARE(first.messages.size(), 20);
        QCOMPARE(second.messages.size(), 20);
        QCOMPARE(third.messages.size(), 5);
        QVERIFY(first.hasMore);
        QVERIFY(second.hasMore);
        QVERIFY(!third.hasMore);

        QStringList contents;
        // 按时间顺序拼：第三页（最早）→ 第二页 → 第一页（最新）
        for (const MessagePage* page : {&third, &second, &first}) {
            for (const QJsonValue& value : page->messages)
                contents << value.toObject().value(QStringLiteral("content")).toString();
        }
        QCOMPARE(contents.size(), 45);
        for (int i = 0; i < contents.size(); ++i)
            QCOMPARE(contents.at(i), QStringLiteral("m%1").arg(i + 1));
    }

    void pageClampsLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());
        appendRange(store, QStringLiteral("blender"), 1, 120);

        QCOMPARE(store.messagePage(QStringLiteral("blender"), 0).messages.size(), 20);
        const MessagePage capped = store.messagePage(QStringLiteral("blender"), 1000);
        QCOMPARE(capped.messages.size(), 100);
        QVERIFY(capped.hasMore);
    }

    void emptyAreaReturnsEmptyPage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SessionStore store(dir.path() + QStringLiteral("/sessions.db"));
        QVERIFY(store.open());

        const MessagePage page = store.messagePage(QStringLiteral("blender"));
        QCOMPARE(page.messages.size(), 0);
        QVERIFY(!page.hasMore);
        QCOMPARE(page.nextCursor, qint64(0));
    }
};

QTEST_MAIN(SessionStoreTest)
#include "tst_session_store.moc"
