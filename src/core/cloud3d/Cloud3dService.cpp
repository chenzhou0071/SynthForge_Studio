#include "core/cloud3d/Cloud3dService.h"

#include "core/assets/AssetLibrary.h"
#include "core/cloud3d/Cloud3dProvider.h"
#include "core/cloud3d/CustomProvider.h"
#include "core/cloud3d/HunyuanProvider.h"
#include "core/cloud3d/TripoProvider.h"
#include "core/settings/SettingsStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace {
constexpr int kMaxRetries = 3;

// 落盘名只留 ASCII 字母数字与 -/_：中文会把整段压成一个横杠
QString sanitizeName(const QString& prompt)
{
    QString out;
    for (const QChar c : prompt.left(24)) {
        if (c.unicode() < 128 && c.isLetterOrNumber()) {
            out.append(c);
        } else if (c == QLatin1Char('-') || c == QLatin1Char('_')) {
            out.append(c);
        } else if (!out.isEmpty() && !out.endsWith(QLatin1Char('-'))) {
            out.append(QLatin1Char('-'));
        }
    }
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("model") : out;
}
} // namespace

Cloud3dService::Cloud3dService(SettingsStore* settings, AssetLibrary* library, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_library(library)
{
    m_pollTimer.setInterval(m_pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Cloud3dService::pollOnce);
}

Cloud3dService::~Cloud3dService() = default;

void Cloud3dService::setProviderForTest(std::unique_ptr<Cloud3dProvider> provider)
{
    m_provider = std::move(provider);
    m_providerId = QStringLiteral("test");
    m_testProvider = true;
    bindProvider();
}

void Cloud3dService::setPollIntervalForTest(int ms)
{
    m_pollIntervalMs = ms;
    m_pollTimer.setInterval(ms);
}

void Cloud3dService::setRetryDelayForTest(int ms)
{
    m_retryDelayMs = ms;
}

void Cloud3dService::setTotalTimeoutForTest(int ms)
{
    m_totalTimeoutMs = ms;
}

void Cloud3dService::start(const QString& prompt, const QString& targetFolder)
{
    if (m_running)
        return;

    m_lastPrompt = prompt;
    m_targetFolder = targetFolder;
    m_taskId.clear();
    m_modelUrl.clear();
    m_attempts = 0;
    m_cancelled = false;
    m_failed = false;
    m_running = true;
    m_elapsed.restart();

    if (!m_testProvider)
        createProvider();
    if (!m_provider) {
        finishWithError(QStringLiteral("cloud3d-failed"),
                        QStringLiteral("未配置 3D 服务商"));
        return;
    }
    submitPrompt();
}

bool Cloud3dService::canRetry() const
{
    return !m_running && m_failed;
}

bool Cloud3dService::retry()
{
    if (m_running || !m_failed)
        return false;

    m_running = true;
    m_cancelled = false;
    m_failed = false;
    m_attempts = 0;
    m_elapsed.restart();

    // 已有产物地址 → 只需重下；已有 taskId → 续轮询。两者都不重复提交扣费
    if (!m_modelUrl.isEmpty()) {
        downloadModel(m_modelUrl);
        return true;
    }
    if (!m_taskId.isEmpty()) {
        m_pollTimer.start();
        return true;
    }
    submitPrompt();
    return true;
}

void Cloud3dService::cancel()
{
    if (!m_running)
        return;
    m_cancelled = true;
    m_pollTimer.stop();
    m_running = false;
    m_failed = false;
    emit errorCard(QStringLiteral("user-cancelled"), QString());
}

void Cloud3dService::createProvider()
{
    // 每次新提交都按最新设置重建：用户可能刚在设置里切换过服务商或地址。
    // （重试走 retry()，不经过这里，仍复用原实例续轮询）
    m_providerBound = false; // 新实例必须重新接信号，否则状态回调全丢
    const QJsonObject cloud =
        m_settings ? m_settings->data().value(QStringLiteral("cloud3d")).toObject() : QJsonObject{};
    const QString id = cloud.value(QStringLiteral("provider")).toString(QStringLiteral("tripo"));
    m_providerId = id;

    if (id == QLatin1String("hunyuan")) {
        const QJsonObject cfg = cloud.value(QStringLiteral("hunyuan")).toObject();
        m_provider = std::make_unique<HunyuanProvider>(
            cfg.value(QStringLiteral("secretId")).toString(),
            cfg.value(QStringLiteral("secretKey")).toString(),
            cfg.value(QStringLiteral("region")).toString(QStringLiteral("ap-guangzhou")),
            cfg.value(QStringLiteral("model")).toString());
    } else if (id == QLatin1String("custom")) {
        const QJsonObject cfg = cloud.value(QStringLiteral("custom")).toObject();
        m_provider = std::make_unique<CustomProvider>(
            cfg.value(QStringLiteral("baseUrl")).toString(),
            cfg.value(QStringLiteral("apiToken")).toString());
    } else {
        const QJsonObject cfg = cloud.value(QStringLiteral("tripo")).toObject();
        m_provider = std::make_unique<TripoProvider>(
            cfg.value(QStringLiteral("apiKey")).toString(),
            cfg.value(QStringLiteral("version")).toString());
    }
    bindProvider();
}

void Cloud3dService::bindProvider()
{
    if (!m_provider || m_providerBound)
        return;
    m_providerBound = true;

    connect(m_provider.get(), &Cloud3dProvider::submitted, this,
            [this](const QString& taskId) {
                if (m_cancelled || !m_running)
                    return;
                m_taskId = taskId;
                broadcast(QStringLiteral("queued"), 0);
                m_pollTimer.start();
            });
    connect(m_provider.get(), &Cloud3dProvider::status, this,
            [this](const Cloud3dStatus& status) {
                if (m_cancelled || !m_running)
                    return;
                m_attempts = 0; // 能拿到状态说明链路已恢复
                broadcast(status.state, status.percent);
                if (status.state == QLatin1String("success")) {
                    m_pollTimer.stop();
                    if (status.modelUrl.isEmpty()) {
                        finishWithError(QStringLiteral("cloud3d-failed"),
                                        QStringLiteral("服务返回成功但缺少模型地址"));
                        return;
                    }
                    m_modelUrl = status.modelUrl;
                    downloadModel(status.modelUrl);
                } else if (status.state == QLatin1String("failed")) {
                    m_pollTimer.stop();
                    finishWithError(QStringLiteral("cloud3d-failed"), status.error);
                } else {
                    m_pollTimer.start(); // 串行轮询：本次响应回来后再排下一次
                }
            });
    connect(m_provider.get(), &Cloud3dProvider::failed, this,
            [this](const QString& error) {
                if (m_cancelled || !m_running)
                    return;
                if (error.startsWith(QStringLiteral("retryable:")) && m_attempts < kMaxRetries) {
                    ++m_attempts;
                    scheduleRetry();
                    return;
                }
                const QString prefix = QStringLiteral("retryable:");
                finishWithError(QStringLiteral("cloud3d-failed"),
                                error.startsWith(prefix) ? error.mid(prefix.size()) : error);
            });
}

void Cloud3dService::submitPrompt()
{
    m_provider->submit(m_lastPrompt, QString());
}

void Cloud3dService::pollOnce()
{
    if (m_cancelled || !m_running || m_taskId.isEmpty())
        return;
    m_pollTimer.stop(); // 串行：本次没回来之前不发下一次，避免并发重复查询
    if (m_elapsed.elapsed() > m_totalTimeoutMs) {
        m_pollTimer.stop();
        finishWithError(QStringLiteral("cloud3d-timeout"),
                        QStringLiteral("生成超时（超过 15 分钟）"));
        return;
    }
    m_provider->poll(m_taskId);
}

void Cloud3dService::scheduleRetry()
{
    m_pollTimer.stop();
    QTimer::singleShot(m_retryDelayMs, this, [this]() {
        if (m_cancelled || !m_running)
            return;
        if (!m_taskId.isEmpty()) {
            m_pollTimer.start();
            pollOnce();
        } else {
            submitPrompt(); // 提交阶段就失败：没有 taskId，只能重新提交
        }
    });
}

void Cloud3dService::downloadModel(const QString& url)
{
    QNetworkReply* reply = m_nam.get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty();
        const QString reason = ok ? QString() : reply->errorString();
        reply->deleteLater();
        if (m_cancelled || !m_running)
            return;
        if (!ok) {
            finishWithError(QStringLiteral("cloud3d-download-failed"), reason);
            return;
        }

        const QString path = targetFilePath();
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            finishWithError(QStringLiteral("cloud3d-save-failed"), file.errorString());
            return;
        }
        file.close();

        m_running = false;
        m_failed = false;
        // 来源落盘：文件树据此标「云 · glb」，重启后还在
        const QString relative = m_library->relativeFor(path);
        if (!relative.isEmpty())
            m_library->markCloudAsset(relative, m_providerId);
        emit assetCreated(path);
    });
}

void Cloud3dService::finishWithError(const QString& reason, const QString& message)
{
    m_pollTimer.stop();
    m_running = false;
    m_failed = true;
    emit errorCard(reason, message);
}

void Cloud3dService::broadcast(const QString& state, int percent)
{
    emit progress(m_taskId, state, qBound(0, percent, 100));
}

QString Cloud3dService::targetFilePath() const
{
    QDir dir(m_targetFolder.isEmpty() ? m_library->root()
                                      : m_library->absoluteFor(m_targetFolder));
    dir.mkpath(QStringLiteral("."));

    const QString base = sanitizeName(m_lastPrompt) + QLatin1Char('-')
        + QDateTime::currentDateTime().toString(QStringLiteral("HHmmss"));
    QString path = dir.filePath(base + QStringLiteral(".glb"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir.filePath(base + QLatin1Char('-') + QString::number(n) + QStringLiteral(".glb"));
    return path;
}
