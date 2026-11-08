// syncwatch - shows how drive-sync, the rclone bisync of ~/Drive, is doing.

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>

#include "backend.h"
#include "pluginsetup.h"
#include "statuscommand.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Commands that need no display run without a GUI application, so the
    // bar can call them every few seconds without touching Wayland.
    if (command == "status") {
        QCoreApplication app(argc, argv);
        app.setApplicationName("syncwatch");
        app.setApplicationVersion("0.1.0");
        QTextStream out(stdout);
        QTextStream err(stderr);
        return runStatusCommand(app.arguments().mid(2), BackendPaths::defaults(), out, err);
    }
    // No argument or "open" starts the window. Arguments that start with a
    // dash are Qt's own (-platform, -style) and go to the window too.
    if (!command.isEmpty() && command != "open" && !command.startsWith('-')) {
        QTextStream(stderr) << usageText();
        return 1;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("syncwatch");
    app.setApplicationVersion("0.1.0");

    // Matches the window to syncwatch.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("syncwatch");
    app.setWindowIcon(QIcon::fromTheme("syncwatch"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    // Links the bar widget shipped in the package; enabling it waits for the user.
    PluginSetup pluginSetup;
    pluginSetup.check();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("pluginSetup", &pluginSetup);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
