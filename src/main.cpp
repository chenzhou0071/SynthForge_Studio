#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QFileDialog>
#include <QIcon>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEngineProfile>
#include <QWebEngineUrlScheme>

#include <functional>

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

    // 注入原生目录选择：core 不依赖 QtWidgets，对话框在此用 QFileDialog，选完/取消都回传路径
    core.setFolderPicker([&window](const QString& startDir,
                                   std::function<void(const QString&)> done) {
        auto* dialog = new QFileDialog(&window, QStringLiteral("选择 Unity 工程文件夹"), startDir);
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly, true);
        dialog->setWindowModality(Qt::ApplicationModal);  // 非 exec()，但保持应用级模态
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(dialog, &QDialog::accepted, dialog, [dialog, done] {
            done(dialog->selectedFiles().value(0));
        });
        QObject::connect(dialog, &QDialog::rejected, dialog, [done] { done(QString()); });
        dialog->open();                                   // 不阻塞事件循环
    });

    window.show();
    return app.exec();
}
