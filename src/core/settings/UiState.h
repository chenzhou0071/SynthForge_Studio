#pragma once

#include <QByteArray>
#include <QString>

// 窗口状态单独放 ui.ini：settings.json 会被前端整包重写（settings.save 直接覆盖 data），
// 窗口状态混进去容易被旧副本盖掉。用 QSettings 的 ini 后端，--data-dir 的测试隔离照样成立。
class UiState
{
public:
    explicit UiState(QString dataDir);

    QString path() const { return m_path; }
    QByteArray windowGeometry() const; // 无记录时返回空
    void setWindowGeometry(const QByteArray& geometry);

private:
    QString m_path;
};
