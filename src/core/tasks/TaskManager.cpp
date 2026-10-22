#include "core/tasks/TaskManager.h"

#include <QStringList>

#include <utility>

int TaskManager::runningCount(const QString& area) const
{
    return (m_running.contains(area) ? 1 : 0) + m_queues.value(area).size();
}

void TaskManager::enqueue(Task task)
{
    const QString area = task.area;
    m_tasks.insert(task.id, task);
    m_queues[area].enqueue(std::move(task));
    pump(area);
}

void TaskManager::pump(const QString& area)
{
    if (area.isEmpty() || m_running.contains(area) || m_queues[area].isEmpty())
        return;
    const Task task = m_queues[area].dequeue();
    m_running.insert(area, task.id);
    emit started(task.id);
    task.run(task.id, this);
}

void TaskManager::finish(const QString& taskId)
{
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return; // 重复 finish / 未知 id：忽略，避免重复信号
    const Task task = it.value();
    m_tasks.erase(it);

    const bool cancelled = m_cancelled.remove(taskId);
    if (m_running.value(task.area) == taskId)
        m_running.remove(task.area);
    emit finished(taskId, cancelled);
    pump(task.area);
}

void TaskManager::cancelArea(const QString& area)
{
    const QString runningId = m_running.value(area);
    if (!runningId.isEmpty())
        m_cancelled.insert(runningId); // 运行中任务：协作式取消

    auto& queue = m_queues[area];
    while (!queue.isEmpty()) {
        const Task task = queue.dequeue();
        m_tasks.remove(task.id);
        emit finished(task.id, true); // 未启动任务：直接结束
    }
}

bool TaskManager::isCancelled(const QString& taskId) const
{
    return m_cancelled.contains(taskId);
}

void TaskManager::cancelProject(const QString& projectId)
{
    QStringList areas;
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it)
        if (it->projectId == projectId && !areas.contains(it->area))
            areas << it->area; // 先收集，避免取消过程中迭代器失效
    for (const QString& area : areas)
        cancelArea(area);
}
