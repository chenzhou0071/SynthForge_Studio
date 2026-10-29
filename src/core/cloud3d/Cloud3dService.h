#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

class AssetLibrary;
class Cloud3dProvider;
class SettingsStore;

// 云 3D 编排：按设置选服务商 → 提交 → 定时轮询 → 下载 GLB → 落盘到资产库。
// provider 只管协议，重试/取消/超时/命名都在这里统一处理。
class Cloud3dService : public QObject
{
    Q_OBJECT
public:
    Cloud3dService(SettingsStore* settings, AssetLibrary* library, QObject* parent = nullptr);
    ~Cloud3dService() override;

    // targetFolder 为资产库相对目录，空串 = 库根
    void start(const QString& prompt, const QString& targetFolder);
    bool canRetry() const;
    bool retry(); // 已拿到 taskId 只续轮询，不重复提交
    void cancel();
    bool isRunning() const { return m_running; }

    // 测试注入：跳过 settings 构造 provider，并缩短轮询/重试间隔
    void setProviderForTest(std::unique_ptr<Cloud3dProvider> provider);
    void setPollIntervalForTest(int ms);
    void setRetryDelayForTest(int ms);
    void setTotalTimeoutForTest(int ms);

signals:
    void progress(const QString& taskId, const QString& state, int percent);
    void assetCreated(const QString& glbPath);
    void errorCard(const QString& reason, const QString& message);

private:
    void createProvider();
    void bindProvider();
    void submitPrompt();
    void pollOnce();
    void scheduleRetry();
    void downloadModel(const QString& url);
    void finishWithError(const QString& reason, const QString& message);
    void broadcast(const QString& state, int percent);
    QString targetFilePath() const;

    SettingsStore* m_settings;
    AssetLibrary* m_library;
    std::unique_ptr<Cloud3dProvider> m_provider;
    QNetworkAccessManager m_nam;
    QTimer m_pollTimer;
    QElapsedTimer m_elapsed;
    QString m_lastPrompt;
    QString m_providerId; // 落盘时把来源（tripo/hunyuan/custom）记进资产库元数据
    QString m_targetFolder;
    QString m_taskId;
    QString m_modelUrl;
    int m_attempts = 0;
    int m_pollIntervalMs = 3000;
    int m_retryDelayMs = 5000;
    qint64 m_totalTimeoutMs = 15 * 60 * 1000; // 整体预算：从提交起算 15 分钟
    bool m_running = false;
    bool m_cancelled = false;
    bool m_failed = false;
    bool m_providerBound = false;
    bool m_testProvider = false;
};
