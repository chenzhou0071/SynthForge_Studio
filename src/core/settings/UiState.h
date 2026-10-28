#pragma once

#include <QByteArray>
#include <QString>

// 窗口状态与界面偏好单独放 ui.ini：settings.json 会被前端整包重写（settings.save 直接覆盖 data），
// 窗口状态混进去容易被旧副本盖掉。用 QSettings 的 ini 后端，--data-dir 的测试隔离照样成立。
class UiState
{
public:
    explicit UiState(QString dataDir);

    QString path() const { return m_path; }
    QByteArray windowGeometry() const; // 无记录时返回空
    void setWindowGeometry(const QByteArray& geometry);

    // 输入区的两个开关：localStorage 在 app:// 下不落盘，只能由后端记住
    QString blenderMode() const; // lowpoly | cloud3d，缺失/未知回落 lowpoly
    void setBlenderMode(const QString& mode);
    QString chatMode() const; // ask | exec，缺失/未知回落 exec
    void setChatMode(const QString& mode);

private:
    QString m_path;
};
