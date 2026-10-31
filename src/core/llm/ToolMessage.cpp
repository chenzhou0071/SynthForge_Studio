#include "core/llm/ToolMessage.h"

#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QIODevice>

namespace {

// 截图为 image content 时压成 JPEG base64：同一份结果既回喂模型又给前端展示
QByteArray compressImage(const QByteArray& png)
{
    QImage image;
    if (!image.loadFromData(png))
        return {};
    if (image.width() > 1024 || image.height() > 1024)
        image = image.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return out;
}

} // namespace

QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error, QStringList* images)
{
    QJsonArray content;
    if (!error.isEmpty()) {
        content.append(QJsonObject{
            {"type", "text"},
            {"text", "ERROR: " + QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact))}});
    } else {
        for (const QJsonValue& item : result.toObject().value(QStringLiteral("content")).toArray()) {
            const QJsonObject obj = item.toObject();
            if (obj.value(QStringLiteral("type")).toString() == QLatin1String("image")) {
                const QByteArray jpeg = compressImage(
                    QByteArray::fromBase64(obj.value(QStringLiteral("data")).toString().toUtf8()));
                if (jpeg.isEmpty())
                    continue;
                const QString dataUrl =
                    QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64());
                content.append(QJsonObject{{"type", "image_url"},
                                           {"image_url", QJsonObject{{"url", dataUrl}}}});
                if (images)
                    images->append(dataUrl);
            } else {
                content.append(obj);
            }
        }
    }
    return QJsonObject{{"role", "tool"}, {"tool_call_id", callId}, {"content", content}};
}

namespace {

bool mentionsDisconnect(const QString& text)
{
    static const QStringList needles{
        QStringLiteral("未找到 unity 编辑器实例"),
        QStringLiteral("unity 编辑器实例"),
        QStringLiteral("editor instance"),
        QStringLiteral("connection refused"),
        QStringLiteral("connection reset"),
        QStringLiteral("not connected"),
        QStringLiteral("disconnected"),
        QStringLiteral("桥接未连"),
        QStringLiteral("无法连接编辑器"),
        QStringLiteral("拒绝连接"),
    };
    for (const QString& needle : needles) {
        if (text.contains(needle, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

bool isRetryableBridgeError(const QJsonValue& result, const QJsonObject& error)
{
    if (!error.isEmpty()) {
        // JSON-RPC 层错误：message 里带连接类描述时重试；invalid arguments 之类不算
        return mentionsDisconnect(error.value(QStringLiteral("message")).toString())
               || mentionsDisconnect(QString::fromUtf8(
                   QJsonDocument(error).toJson(QJsonDocument::Compact)));
    }
    const QJsonObject obj = result.toObject();
    QString text;
    for (const QJsonValue& item : obj.value(QStringLiteral("content")).toArray())
        text += item.toObject().value(QStringLiteral("text")).toString() + QLatin1Char('\n');
    return mentionsDisconnect(text);
}
