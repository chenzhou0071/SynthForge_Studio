#pragma once

#include <QByteArray>
#include <QWebEngineUrlSchemeHandler>

class AppSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    explicit AppSchemeHandler(QObject* parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob* job) override;

private:
    static QByteArray mimeForPath(const QString& path);
};
