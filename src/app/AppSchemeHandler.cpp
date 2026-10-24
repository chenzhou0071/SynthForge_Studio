#include "app/AppSchemeHandler.h"

#include <QFile>
#include <QWebEngineUrlRequestJob>

AppSchemeHandler::AppSchemeHandler(QObject* parent)
    : QWebEngineUrlSchemeHandler(parent)
{
}

void AppSchemeHandler::requestStarted(QWebEngineUrlRequestJob* job)
{
    QString path = job->requestUrl().path();
    if (path.isEmpty() || path == QStringLiteral("/"))
        path = QStringLiteral("/index.html");

    // app:// 只从 qrc 取文件，不触碰真实文件系统
    auto* file = new QFile(QStringLiteral(":/web") + path, job);
    if (!file->open(QIODevice::ReadOnly)) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    job->reply(mimeForPath(path), file);
}

QByteArray AppSchemeHandler::mimeForPath(const QString& path)
{
    if (path.endsWith(QStringLiteral(".js")) || path.endsWith(QStringLiteral(".mjs")))
        return "text/javascript";
    if (path.endsWith(QStringLiteral(".css")))
        return "text/css";
    if (path.endsWith(QStringLiteral(".svg")))
        return "image/svg+xml";
    if (path.endsWith(QStringLiteral(".png")))
        return "image/png";
    if (path.endsWith(QStringLiteral(".jpg")) || path.endsWith(QStringLiteral(".jpeg")))
        return "image/jpeg";
    if (path.endsWith(QStringLiteral(".webp")))
        return "image/webp";
    if (path.endsWith(QStringLiteral(".json")))
        return "application/json";
    if (path.endsWith(QStringLiteral(".woff2")))
        return "font/woff2";
    return "text/html";
}
