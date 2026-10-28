#include <QApplication>
#include <QByteArray>
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

    // 默认走 GPU 合成：软件合成下滚动聊天、拖拽 3D 预览都吃 CPU，建模时更明显。
    // 个别机器拉伸窗口会闪蓝块，可用 --software-render 或 SYNTHFORGE_SOFTWARE_RENDER=1 退回。
    bool softwareRender = qEnvironmentVariable("SYNTHFORGE_SOFTWARE_RENDER") == "1";
    for (int i = 1; i < argc && !softwareRender; ++i)
        softwareRender = QByteArray(argv[i]) == "--software-render";
    if (softwareRender && qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
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

    UiState& uiState = *core.uiState(); // 与 uiState.get/set 共用，避免双实例写同一个文件
    MainWindow window(uiUrl, uiState.windowGeometry(),
                      [&uiState](const QByteArray& geometry) {
                          uiState.setWindowGeometry(geometry);
                      },
                      softwareRender); // core 先声明：退出时窗口先销毁
    window.show();
    return app.exec();
}
