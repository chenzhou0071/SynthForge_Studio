#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QTimer>
#include <QUrl>

#include <functional>

class QWebEngineView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // savedGeometry: 上次 saveGeometry() 的内容；为空则首次启动，用默认尺寸并居中
    // onGeometrySave: 关窗时回传几何，落盘由 main 里的 UiState 负责
    // softwareRender: 软件合成时才需要等缩放停手再对齐，GPU 合成下实时跟随
    MainWindow(const QUrl& uiUrl, QByteArray savedGeometry,
               std::function<void(const QByteArray&)> onGeometrySave, bool softwareRender,
               QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void syncViewGeometry();

    QWidget* m_host;
    QWebEngineView* m_view;
    QTimer m_resizeTimer; // 仅软件合成时用：拖动停手后才把网页尺寸对齐
    bool m_softwareRender = false;
    bool m_centerOnFirstShow = false;
    std::function<void(const QByteArray&)> m_onGeometrySave;
};
