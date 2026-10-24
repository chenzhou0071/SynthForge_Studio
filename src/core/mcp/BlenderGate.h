#pragma once

#include <QString>

// Blender MCP 会话的单 owner 租约：低模 / 修改任务与 unity 区的 glb→fbx 转换共用同一个
// Blender 会话，而转换会 read_factory_settings(use_empty=True) 清空场景，必须互斥。
// 非 QObject，只在 CoreApp 所在的 UI 线程使用。
class BlenderGate
{
public:
    bool tryAcquire(const QString& owner);
    void release(const QString& owner);
    bool isBusy() const { return !m_owner.isEmpty(); }
    QString owner() const { return m_owner; }

private:
    QString m_owner;
};
