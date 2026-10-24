#include "core/mcp/BlenderGate.h"

bool BlenderGate::tryAcquire(const QString& owner)
{
    if (!m_owner.isEmpty())
        return false;
    m_owner = owner;
    return true;
}

void BlenderGate::release(const QString& owner)
{
    // 只有持有者能释放：避免一方误放掉另一方的租约
    if (m_owner == owner)
        m_owner.clear();
}
