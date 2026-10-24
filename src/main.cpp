#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEngineProfile>
#include <QWebEngineUrlScheme>

#include "app/AppSchemeHandler.h"
#include "app/MainWindow.h"
#include "core/coreapp/CoreApp.h"
#include "core/settings/SettingsStore.h"
#include "core/settings/UiState.h"
#include "core/ws/WsServer.h"

int main(int argc, char* argv[])
{
    QWebEngineUrlScheme scheme(QByteArrayLiteral("app"));
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme); // 必须先于 QApplication

    // Intel 核显 + D3D11 合成：拉伸窗口、首次流式重绘时会闪蓝色色块。
    // 降一档到软件合成，聊天界面开销可忽略；用户显式设置时以用户为准。
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu-compositing");

    QApplication app(argc, argv);
    // 规格固定 %APPDATA%/SynthForgeStudio：不能设 organizationName，
    // 否则 AppDataLocation 会变成 %APPDATA%/SynthForge/SynthForgeStudio
    QCoreApplication::setApplicationName(QStringLiteral("SynthForgeStudio"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png"))); // qrc 见 CMake app_icons

    const QStringList args = QCoreApplication::arguments();
    const int devIdx = args.indexOf(QStringLiteral("--dev-url"));
    QUrl uiUrl = devIdx >= 0 && devIdx + 1 < args.size()
                     ? QUrl(args.at(devIdx + 1))
                     : QUrl(QStringLiteral("app://ui/index.html"));

    const QString dataDir = dataDirFromArgs(args);
    CoreApp core({dataDir, false});
    if (!core.start())
        return 1;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ws"),
                       QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                           .arg(core.ws()->port())
                           .arg(core.ws()->token()));
    uiUrl.setQuery(query); // 前端 ?ws= 的唯一来源

    auto* handler = new AppSchemeHandler(&app);
    QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(QByteArrayLiteral("app"),
                                                                 handler);

    UiState uiState(dataDir);
    MainWindow window(uiUrl, uiState.windowGeometry(),
                      [&uiState](const QByteArray& geometry) {
                          uiState.setWindowGeometry(geometry);
                      }); // core 先声明：退出时窗口先销毁
    window.show();
    return app.exec();
}
