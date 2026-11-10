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
#include "controlcommands.h"
#include "notifier.h"
#include "pluginsetup.h"
#include "statuscommand.h"
#include "takeover.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Commands that need no display run without a GUI application, so the
    // bar and the units can call them without touching Wayland.
    static const QStringList headless{"status", "sync", "pause", "resume", "run-condition", "run", "notify",
                                      "watchdog", "takeover"};
    if (headless.contains(command)) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("syncwatch");
        app.setApplicationVersion("0.1.0");
        QTextStream out(stdout);
        QTextStream err(stderr);
        const QStringList rest = app.arguments().mid(2);
        const BackendPaths paths = BackendPaths::defaults();
        if (command == "status")
            return runStatusCommand(rest, paths, out, err);
        if (command == "sync")
            return runSyncCommand(rest, paths, out, err);
        if (command == "pause")
            return runPauseCommand(rest, paths, QDateTime::currentDateTimeUtc(), out, err);
        if (command == "resume")
            return runResumeCommand(rest, paths, out, err);
        if (command == "run")
            return runRunCommand(paths, err);
        if (command == "takeover")
            return runTakeoverCommand(rest, TakeoverPaths::defaults(), paths, out, err);
        if (command == "notify" || command == "watchdog") {
            // The cards are read by a person, in the Omarchy language.
            Translator translator;
            app.installTranslator(&translator);
            const NotifierPaths tools = NotifierPaths::defaults(Translator::formatLocale(translator.language()));
            const QDateTime now = QDateTime::currentDateTimeUtc();
            if (command == "notify")
                return runNotifyCommand(qEnvironmentVariable("SERVICE_RESULT"), paths, tools, now);
            QString error;
            const auto config = loadSyncConfig(paths.config, &error);
            return runWatchdogCommand(paths, config.value_or(SyncConfig()), tools, now, config ? QString() : error);
        }
        // run-condition: 255 is not a skip for systemd but a failure, which
        // is what a missing or broken config is.
        QString error;
        const auto config = loadSyncConfig(paths.config, &error);
        if (!config) {
            err << "syncwatch: " << error << "\n";
            return 255;
        }
        return runConditionCommand(paths, *config, QDateTime::currentDateTimeUtc());
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
    // Asks this same program, in a child process, whether drive-sync can be taken over.
    TakeoverSetup takeoverSetup(QCoreApplication::applicationFilePath());
    takeoverSetup.check();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("pluginSetup", &pluginSetup);
    engine.rootContext()->setContextProperty("takeoverSetup", &takeoverSetup);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
