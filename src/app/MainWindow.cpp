#include "app/MainWindow.h"

#include <QCloseEvent>
#include <QColor>
#include <QPalette>
#include <QResizeEvent>
#include <QScreen>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <utility>

MainWindow::MainWindow(const QUrl& uiUrl, QByteArray savedGeometry,
                       std::function<void(const QByteArray&)> onGeometrySave, QWidget* parent)
    : QMainWindow(parent)
    , m_host(new QWidget(this))
    , m_view(new QWebEngineView(m_host))
    , m_onGeometrySave(std::move(onGeometrySave))
{
    setWindowTitle(QStringLiteral("SynthForge Studio"));
    // 再窄三栏就挤成一团（模型库 300 + 预览 + 对话 400），再矮聊天区放不下输入框
    setMinimumSize(1000, 625);
    // 首次启动：先用默认尺寸，具体位置等 show 之后按外框居中
    if (!restoreGeometry(savedGeometry)) {
        resize(1440, 900);
        m_centerOnFirstShow = true;
    }

    // 与 styles.css 的 --bg 一致：网页没盖到的区域也是同色，不会露出白/蓝
    QPalette palette = m_host->palette();
    palette.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
    m_host->setAutoFillBackground(true);
    m_host->setPalette(palette);

    m_view->page()->setBackgroundColor(QColor(0x1e, 0x1e, 0x1e));
    m_view->setUrl(uiUrl);

    // Qt WebEngine 的合成表面跟不上窗口尺寸：拖动时直接缩放会露出
    // 未初始化的底色（Intel 核显上表现为蓝色块）。先把网页尺寸冻住，
    // 停手 120ms 后再一次性对齐，页面只重排一次。
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(120);
    connect(&m_resizeTimer, &QTimer::timeout, this, &MainWindow::syncViewGeometry);

    setCentralWidget(m_host);
    syncViewGeometry();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    m_resizeTimer.start(); // 每次尺寸变化都重置，拖动期间不落定
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 最大化时 saveGeometry() 带状态位，下次 restoreGeometry() 能直接还原成最大化
    if (m_onGeometrySave)
        m_onGeometrySave(saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!m_centerOnFirstShow)
        return;
    m_centerOnFirstShow = false;
    // show 之后 frameGeometry 才包含标题栏/边框；按内容区算会整体偏下半个标题栏
    const QRect available = screen()->availableGeometry();
    const QSize frame = frameGeometry().size();
    move(available.center() - QPoint(frame.width() / 2, frame.height() / 2));
}

void MainWindow::syncViewGeometry()
{
    m_view->setGeometry(m_host->rect());
}
