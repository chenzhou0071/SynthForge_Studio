#pragma once

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

    // 虚函数：P4 的 MockLlmServer 继承重写，按脚本喂真实 SSE 分片；生产只有这一实现
    virtual void chatStream(const LlmConfig& cfg, const QJsonArray& messages,
                            const QJsonArray& tools, StreamHandlers handlers);
    void abort(); // 中止所有在途流（取消任务时用），每个流会走 onError

private:
    QNetworkAccessManager* m_nam;
    QSet<QNetworkReply*> m_replies;
};
