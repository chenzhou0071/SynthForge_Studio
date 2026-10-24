#include <QtTest>

#include "core/mcp/BlenderGate.h"

class BlenderGateTest : public QObject
{
    Q_OBJECT
private slots:
    void acquireAndRelease()
    {
        BlenderGate gate;
        QVERIFY(!gate.isBusy());
        QVERIFY(gate.tryAcquire(QStringLiteral("blender-agent")));
        QVERIFY(gate.isBusy());
        QCOMPARE(gate.owner(), QStringLiteral("blender-agent"));
        QVERIFY(!gate.tryAcquire(QStringLiteral("unity-convert"))); // 不可抢占
        gate.release(QStringLiteral("unity-convert"));               // 非 owner 释放无效
        QVERIFY(gate.isBusy());
        gate.release(QStringLiteral("blender-agent"));
        QVERIFY(!gate.isBusy());
        QVERIFY(gate.tryAcquire(QStringLiteral("unity-convert")));   // 释放后可再获取
    }
};

QTEST_MAIN(BlenderGateTest)
#include "tst_blender_gate.moc"
