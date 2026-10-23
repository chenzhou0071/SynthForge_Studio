#include "core/store/SessionStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>

#include <utility>

SessionStore::SessionStore(QString dbPath)
    : m_path(std::move(dbPath))
{
    m_connectionName =
        QStringLiteral("sessions_%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
}

SessionStore::~SessionStore()
{
    if (!m_db.isValid())
        return;
    const QString name = m_db.connectionName();
    m_db.close();
    m_db = QSqlDatabase(); // 必须先释放句柄，否则 removeDatabase 会警告连接仍在使用
    QSqlDatabase::removeDatabase(name);
}

bool SessionStore::open()
{
    if (m_db.isOpen())
        return true;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_path);
    if (!m_db.open())
        return false;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL")); // 防断电损坏 + 读写并发
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS messages("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT, area TEXT NOT NULL,"
                          "role TEXT NOT NULL, content TEXT NOT NULL, ts INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS drafts("
                          "area TEXT PRIMARY KEY, content TEXT NOT NULL, updated INTEGER NOT NULL)"));
    // 分页查询走 (area, id)：定位到某个区、某条 id 之前，不扫其他区
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_area_id "
                          "ON messages(area, id)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS tasks("
                          "id TEXT PRIMARY KEY, area TEXT NOT NULL, kind TEXT NOT NULL,"
                          "state TEXT NOT NULL, payload TEXT NOT NULL,"
                          "result TEXT NOT NULL DEFAULT '{}', updated INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS assets("
                          "id TEXT PRIMARY KEY, area TEXT NOT NULL, path TEXT NOT NULL,"
                          "card TEXT NOT NULL, updated INTEGER NOT NULL)"));
    return true;
}

void SessionStore::appendMessage(const QString& area, const QString& role,
                                 const QString& content)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO messages(area, role, content, ts) VALUES(?,?,?,?)"));
    q.addBindValue(area);
    q.addBindValue(role);
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::messages(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT role, content FROM messages WHERE area=? ORDER BY id ASC"));
    q.addBindValue(area);
    q.exec();
    while (q.next())
        out.append(QJsonObject{{QStringLiteral("role"), q.value(0).toString()},
                               {QStringLiteral("content"), q.value(1).toString()}});
    return out;
}

MessagePage SessionStore::messagePage(const QString& area, int limit, qint64 beforeId) const
{
    MessagePage page;
    const int pageSize = limit <= 0 ? 20 : qMin(limit, 100);

    QSqlQuery q(m_db);
    if (beforeId > 0) {
        q.prepare(QStringLiteral("SELECT id, role, content, ts FROM messages "
                                 "WHERE area=? AND id<? ORDER BY id DESC LIMIT ?"));
        q.addBindValue(area);
        q.addBindValue(beforeId);
        q.addBindValue(pageSize + 1); // 多取一条，用来判断还有没有更早的
    } else {
        q.prepare(QStringLiteral("SELECT id, role, content, ts FROM messages "
                                 "WHERE area=? ORDER BY id DESC LIMIT ?"));
        q.addBindValue(area);
        q.addBindValue(pageSize + 1);
    }
    q.exec();

    QJsonArray newestFirst;
    while (q.next()) {
        newestFirst.append(QJsonObject{
            {QStringLiteral("id"), q.value(0).toLongLong()},
            {QStringLiteral("role"), q.value(1).toString()},
            {QStringLiteral("content"), q.value(2).toString()},
            {QStringLiteral("ts"), q.value(3).toLongLong()}});
    }
    page.hasMore = newestFirst.size() > pageSize;
    if (page.hasMore)
        newestFirst.removeLast(); // 探测用的那条不属于本页

    // 查询是倒序，反转成旧→新，前端直接往列表尾部/顶部拼接
    for (int i = newestFirst.size() - 1; i >= 0; --i)
        page.messages.append(newestFirst.at(i));
    if (!page.messages.isEmpty())
        page.nextCursor = page.messages.first().toObject().value(QStringLiteral("id")).toInteger();
    return page;
}

void SessionStore::saveDraft(const QString& area, const QString& content)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO drafts(area, content, updated) VALUES(?,?,?)"
                             " ON CONFLICT(area) DO UPDATE SET content=excluded.content,"
                             " updated=excluded.updated"));
    q.addBindValue(area);
    q.addBindValue(content);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QString SessionStore::draft(const QString& area) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT content FROM drafts WHERE area=?"));
    q.addBindValue(area);
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

void SessionStore::clearMessages(const QString& area)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM messages WHERE area=?"));
    q.addBindValue(area);
    q.exec();
}

void SessionStore::upsertTask(const QString& id, const QString& area, const QString& kind,
                              const QString& state, const QJsonObject& payload,
                              const QJsonObject& result)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO tasks(id, area, kind, state, payload, result, updated)"
                             " VALUES(?,?,?,?,?,?,?)"
                             " ON CONFLICT(id) DO UPDATE SET state=excluded.state,"
                             " result=excluded.result, updated=excluded.updated"));
    q.addBindValue(id);
    q.addBindValue(area);
    q.addBindValue(kind);
    q.addBindValue(state);
    q.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    q.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::tasks(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, kind, state, payload, result FROM tasks WHERE area=?"
                             " ORDER BY updated DESC LIMIT 50"));
    q.addBindValue(area);
    q.exec();
    while (q.next()) {
        out.append(QJsonObject{
            {"id", q.value(0).toString()},
            {"kind", q.value(1).toString()},
            {"state", q.value(2).toString()},
            {"payload", QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object()},
            {"result", QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object()}});
    }
    return out;
}

void SessionStore::upsertAsset(const QString& id, const QString& area, const QString& path,
                               const QJsonObject& card)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO assets(id, area, path, card, updated)"
                             " VALUES(?,?,?,?,?)"
                             " ON CONFLICT(id) DO UPDATE SET path=excluded.path,"
                             " card=excluded.card, updated=excluded.updated"));
    q.addBindValue(id);
    q.addBindValue(area);
    q.addBindValue(path);
    q.addBindValue(QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QJsonArray SessionStore::assets(const QString& area) const
{
    QJsonArray out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, path, card FROM assets WHERE area=?"
                             " ORDER BY updated DESC"));
    q.addBindValue(area);
    q.exec();
    while (q.next()) {
        out.append(QJsonObject{
            {"id", q.value(0).toString()},
            {"path", q.value(1).toString()},
            {"card", QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object()}});
    }
    return out;
}
