#include "app/MainWindow.h"

#include <QWebEngineView>

MainWindow::MainWindow(const QUrl& uiUrl, QWidget* parent)
    : QMainWindow(parent)
    , m_view(new QWebEngineView(this))
{
    setWindowTitle(QStringLiteral("SynthForge Studio"));
    resize(1440, 900);
    setCentralWidget(m_view);
    m_view->setUrl(uiUrl);
}
