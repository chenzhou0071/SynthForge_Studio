#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>

class QNetworkReply;

// 三家云服务统一的状态：Service 只认这四种 state。
struct Cloud3dStatus
{
    QString state; // queued | running | success | failed
    int percent = 0;
    QString modelUrl; // success 时有效
    QString error;    // failed 时有效
};

Q_DECLARE_METATYPE(Cloud3dStatus)

class Cloud3dProvider : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~Cloud3dProvider() override = default;

    virtual void submit(const QString& prompt, const QString& model) = 0;
    virtual void poll(const QString& taskId) = 0;

signals:
    void submitted(const QString& taskId);
    void status(const Cloud3dStatus& status);
    void failed(const QString& error);

protected:
    // 网络/5xx → "retryable:" 前缀；4xx 与业务错误保留服务端 message
    static QString failureText(QNetworkReply* reply, const QByteArray& body);
};
