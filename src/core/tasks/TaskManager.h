#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <functional>

class TaskManager;

struct Task
{
    QString id;
    QString area;      // "blender" | "unity"
    QString projectId; // unity 区绑定工程，可空
    std::function<void(const QString& taskId, TaskManager*)> run;
};

class TaskManager : public QObject
{
    Q_OBJECT

public:
    void enqueue(Task task);
    void finish(const QString& taskId);
    void cancelArea(const QString& area);
    void cancelProject(const QString& projectId);
    int runningCount(const QString& area) const; // 运行中 + 排队（P6 切换确认用）
    bool isCancelled(const QString& taskId) const;

signals:
    void started(const QString& taskId);
    void finished(const QString& taskId, bool cancelled);

private:
    void pump(const QString& area);

    QHash<QString, QQueue<Task>> m_queues;
    QHash<QString, QString> m_running; // area -> taskId
    QHash<QString, Task> m_tasks;      // taskId -> Task（查 projectId）
    QSet<QString> m_cancelled;         // taskId 集合（不是 area，避免误伤后续任务）
};
