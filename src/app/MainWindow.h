#pragma once

#include <QMainWindow>
#include <QUrl>

class QWebEngineView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QUrl& uiUrl, QWidget* parent = nullptr);

private:
    QWebEngineView* m_view;
};
