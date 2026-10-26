#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

struct MessagePage
{
    QJsonArray messages;   // 旧→新，每项含 id / role / content / ts / meta
    bool hasMore = false;  // 是否还有更早的
    qint64 nextCursor = 0; // 本页最早一条的 id，下一页 beforeId 传它
};

class SessionStore
{
public:
    explicit SessionStore(QString dbPath);
    ~SessionStore();

    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    bool open(); // 建表 + PRAGMA journal_mode=WAL
    qint64 appendMessage(const QString& area, const QString& role, const QString& content,
                         const QJsonObject& meta = QJsonObject{});
    QJsonArray messages(const QString& area) const; // 全量，仅测试/迁移用
    // 游标分页：beforeId <= 0 取最新一页；limit <= 0 用默认 20，上限 100
    MessagePage messagePage(const QString& area, int limit = 20, qint64 beforeId = 0) const;
    QJsonObject messageById(qint64 id) const;
    bool updateMessageContent(qint64 id, const QString& content);
    bool updateMessageMeta(qint64 id, const QJsonObject& meta);
    bool setMessageRole(qint64 id, const QString& role);

    void clearMessages(const QString& area);
    // 任务落库：同 id 幂等覆盖；payload/result 以 JSON 列存储
    void upsertTask(const QString& id, const QString& area, const QString& kind,
                    const QString& state, const QJsonObject& payload,
                    const QJsonObject& result = QJsonObject{});
    QJsonArray tasks(const QString& area) const; // 最近 50 条，updated 降序
    // 资产落库：同 id 幂等覆盖；card 为资产卡片 JSON
    void upsertAsset(const QString& id, const QString& area, const QString& path,
                     const QJsonObject& card);
    QJsonArray assets(const QString& area) const; // 旧→新

private:
    QString m_path;
    QString m_connectionName; // 每个实例独立连接名，避免同进程多实例互相顶掉
    QSqlDatabase m_db;
};
