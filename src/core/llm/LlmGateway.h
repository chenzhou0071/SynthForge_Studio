#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

struct LlmConfig
{
    QString baseUrl;
    QString apiKey;
    QString model;
};

class LlmGateway : public QObject
{
    Q_OBJECT

public:
    struct StreamHandlers
    {
        std::function<void(const QString& delta)> onDelta;
        std::function<void(const QJsonArray& toolCalls)> onToolCalls;
        std::function<void(const QString& error)> onError;
        std::function<void()> onDone;
    };

    explicit LlmGateway(QObject* parent = nullptr);

    // 注：virtual 为 P2 遗留，P4 Task 1B 会移除；禁止子类 mock（测试一律走真实 HTTP + SSE）
    virtual void chatStream(const LlmConfig& cfg, const QJsonArray& messages,
                            const QJsonArray& tools, StreamHandlers handlers,
                            const QString& tag = QString());
    void abort();               // 中止所有在途流（退出时用），每个流会走 onError
    void abortTag(const QString& tag); // 只中止该标签的流（按区取消用）

private:
    QNetworkAccessManager* m_nam;
    QSet<QNetworkReply*> m_replies;
    QHash<QNetworkReply*, QString> m_tags; // reply -> 调用方标签（通常是区名）
};
