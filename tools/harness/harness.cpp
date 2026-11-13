// The DriveSync window, offscreen, against a made-up drive-sync. It loads the
// real Main.qml with a fake log, a fake systemctl, a fake omarchy and a fake
// drivesync, presses keys, and saves what the window shows as a PNG. Nothing
// opens on the desktop and nothing of this machine is read (unless --real),
// so the pictures can go into the README.
//
//   bin/harness --out build/shot.png [options]   (build/shot.png by default)
//   --lang en|pt-BR                      the Omarchy language
//   --scenario ok|running|failed|clean|empty|maxdelete   clean: no conflict copies; empty: no run in the log;
//                                        maxdelete: the last run stopped at the guard, 2 + 1800 deletions
//   --clock "<Qt format>"                the bar clock; without it, the system locale's hours
//   --pause none|hour|forever|ended      a pause mark; --managed: DriveSync's own units
//   --takeover none|offer|running|blocked|failed
//   --bar none|offer|blocked             what the fake omarchy says about the widget
//   --fix none|fixing|confirm|questions|failed   a fix at that step (implies --managed);
//                                        confirm comes with a dry run of 1200 copies
//   --fix-later                          writes that fix after the window is up, so it opens by itself
//   --keys <letters>                     t Tab, digits (1 and 2 the tabs), h ?, d Down, e Esc,
//                                        s Space, p P, o T, f F, D D, c C, l L, x X, r Enter,
//                                        b Backspace, w waits a second
//   --width N --height N                 1000x700 by default
//   --real                               this machine's drive-sync: never for the README

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "clockformat.h"
#include "fixstate.h"
#include "pausestate.h"
#include "pluginsetup.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(path));
    f.write(data);
}

static QString L(const QDateTime &t, const QString &level, const QString &m) {
    return QString("%1 %2: %3\n").arg(t.toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), m);
}
static QString C(const QString &tag, const QString &what, const QString &path) {
    return QString("- %1%2 - %3").arg(tag.leftJustified(18), what.leftJustified(43), path);
}
static const QString kStart = R"(Synching Path1 "gdrive{ab12}:/" with Path2 "/home/someone/Drive/")";

static QString okRun(const QDateTime &s, int secs, const QStringList &cloud, const QStringList &notebook) {
    QString log = L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
    log += L(s.addSecs(secs - 20), "INFO", "Path1 checking for diffs");
    for (const auto &p : cloud) log += L(s.addSecs(secs - 20), "INFO", C("Path1", "File changed: size (larger), time (newer)", p));
    log += L(s.addSecs(secs - 20), "INFO", QString("Path1:    %1 changes:    0 new,    %1 modified,    0 deleted").arg(cloud.size()));
    log += L(s.addSecs(secs - 20), "INFO", "Path2 checking for diffs");
    for (const auto &p : notebook) log += L(s.addSecs(secs - 20), "INFO", C("Path2", "File is new", p));
    log += L(s.addSecs(secs - 20), "INFO", QString("Path2:    %1 changes:    %1 new,    0 modified,    0 deleted").arg(notebook.size()));
    log += L(s.addSecs(secs - 19), "INFO", "Applying changes");
    log += L(s.addSecs(secs), "INFO", "Bisync successful");
    return log;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    auto opt = [&](const QString &n, const QString &d) { int i = args.indexOf(n); return i >= 0 && i + 1 < args.size() ? args[i + 1] : d; };
    const QString lang = opt("--lang", "en"), scenario = opt("--scenario", "ok"), keys = opt("--keys", ""), out = opt("--out", "build/shot.png");
    const int width = opt("--width", "1000").toInt(), height = opt("--height", "700").toInt();

    QTemporaryDir dir;
    put(dir.filePath("language"), lang.toUtf8() + "\n");
    const QDateTime now = QDateTime::currentDateTime();

    QString log;
    QStringList cloudFiles{"Notes/ideas.md", "Documents/budget-2026.ods"};
    QStringList localFiles{"GitHub/my-app/README.md", "GitHub/my-app/src/main.mjs", "Photos/trip/beach-01.jpg"};
    for (int i = 6; i >= 1; --i)
        log += okRun(now.addSecs(-i * 900 - 500), 470 + i * 7, i % 2 ? cloudFiles.mid(0, 1) : cloudFiles,
                     i == 1 ? localFiles : i == 3 ? QStringList{"Notes/groceries.md"} : QStringList{});
    QString active = "inactive";
    if (scenario == "failed") {
        const QDateTime s = now.addSecs(-200);
        log += L(s, "INFO", kStart);
        log += L(s, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
        log += L(s, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
    } else if (scenario == "running") {
        const QDateTime s = now.addSecs(-250);
        log += L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
        log += L(now.addSecs(-10), "INFO", "");
        log += "Transferred:   \t 0 B / 0 B, -, 0 B/s, ETA -\nChecks:  41000 / 41000, 100%, Listed 41210\nElapsed time:  4m0.0s\n\n";
        active = "activating";
    } else if (scenario == "maxdelete") {
        // What rclone 1.75.2 writes when --max-delete stops a run: each
        // deletion, both sides, then the abort for the side over the guard.
        const QDateTime s = now.addSecs(-200);
        log += L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
        log += L(s, "INFO", "Path1 checking for diffs");
        for (const QString &p : {QString("Notes/old-draft.md"), QString("Documents/budget-2025.ods")})
            log += L(s, "INFO", C("Path1", "File was deleted", p));
        log += L(s, "INFO", "Path1:    2 changes:    0 new,    0 modified,    2 deleted");
        log += L(s, "INFO", "Path2 checking for diffs");
        for (int i = 0; i < 1800; ++i)
            log += L(s, "INFO", C("Path2", "File was deleted", QString("Photos/2024/trip-%1.jpg").arg(i, 4, 10, QChar('0'))));
        log += L(s, "INFO", "Path2: 1800 changes:    0 new,    0 modified, 1800 deleted");
        log += L(s, "ERROR", R"(Safety abort: too many deletes (>5%, 1800 of 33411) on Path2 "/home/someone/Drive/". Run with --force if desired.)");
        log += L(s, "NOTICE", "Bisync aborted. Please try again.");
        log += L(s, "NOTICE", "Failed to bisync: too many deletes");
    }
    if (scenario == "empty")
        log.clear();  // a new or just rotated log
    put(dir.filePath("drive-sync.log"), log.toUtf8());

    if (scenario != "clean" && scenario != "empty") {
        put(dir.filePath("Drive/Notes/ideas.md.conflict1"), QByteArray(2400, 'x'));
        put(dir.filePath("Drive/Spreadsheets/bills.xlsx.conflict1"), QByteArray(18300, 'x'));
    } else {
        QDir().mkpath(dir.filePath("Drive"));
    }

    const QString fix = opt("--fix", "none");
    const QString systemctl = dir.filePath("systemctl");
    put(systemctl, QString("#!/bin/sh\ncase \"$*\" in\n  *list-timers*) echo '[{\"next\":%1}]' ;;\n"
                           "  *) printf 'LoadState=loaded\\nActiveState=%2\\nUnitFileState=static\\nResult=success\\nExecMainStartTimestamp=@%3\\n\\n"
                           "LoadState=loaded\\nActiveState=active\\nUnitFileState=enabled\\nResult=success\\n\\n"
                           "LoadState=loaded\\nActiveState=%4\\nUnitFileState=static\\nResult=success\\n' ;;\nesac\n")
                       .arg(now.addSecs(380).toMSecsSinceEpoch() * 1000).arg(active).arg(now.addSecs(-1400).toSecsSinceEpoch())
                       .arg(fix == "fixing" ? "activating" : "inactive").toUtf8());
    QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QQuickStyle::setStyle("Material");
    Translator translator(dir.filePath("language"));
    app.installTranslator(&translator);
    Theme theme(dir.filePath("omarchy"));
    const bool real = args.contains("--real");
    BackendPaths fake{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                      dir.filePath("proc"), systemctl, QStandardPaths::findExecutable("find"), dir.filePath("app"),
                      QString(), QString()};
    fake.managed = args.contains("--managed") || fix != "none";
    const bool fixLater = args.contains("--fix-later");
    auto writeTheFix = [&] {
        FixState state;
        state.step = fix == "fixing" ? "base" : fix;
        state.started = QDateTime::currentDateTimeUtc().addSecs(-1500);
        state.stepStarted = QDateTime::currentDateTimeUtc().addSecs(-300);
        if (fix == "confirm") {
            // A big dry run: the dialog lists the first 500 and counts the rest.
            QByteArray dryRun =
                "2026/11/11 12:00:01 NOTICE: - Path1             Resync is copying files to                  - Path2\n"
                "2026/11/11 12:00:01 NOTICE: Notes/old-draft.md: Skipped copy as --dry-run is set (size 2400)\n"
                "2026/11/11 12:00:01 NOTICE: Documents/budget-2026.ods: Skipped copy as --dry-run is set (size 18300)\n"
                "2026/11/11 12:00:01 NOTICE: - Path2             Resync is copying files to                  - Path1\n";
            for (int i = 0; i < 1200; ++i)
                dryRun += QString("2026/11/11 12:00:01 NOTICE: Photos/2025/trip-%1.jpg: Skipped copy as --dry-run is set "
                                  "(size 2400000)\n").arg(i, 4, 10, QChar('0')).toUtf8();
            put(fixDryRunLogPath(fake.appState), dryRun);
            state.copies = 1202;
        }
        if (fix == "questions") {
            const qint64 t = QDateTime::currentSecsSinceEpoch();
            FixQuestion edited{"Notes/ideas.md", "differs", 2600, 2400, t - 1300, t - 5400, ""};
            FixQuestion added{"Documents/budget-2026.ods", "onlyCloud", 18300, -1, t - 900, 0, ""};
            FixQuestion sheet{"Spreadsheets/bills.xlsx", "differs", 19100, 18300, t - 1100, t - 86400, ""};
            state.questions = {added, edited, sheet};
        }
        if (fix == "failed") {
            state.failedStep = "base";
            state.error = "Bisync critical error: prior lock file found: ~/.local/state/rclone/bisync/gdrive_..lck";
        }
        writeFix(fake.appState, state);
    };
    if (fix != "none" && !fixLater)
        writeTheFix();
    const QString pause = opt("--pause", "none");
    if (pause != "none") {
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc().addSecs(-1800);
        if (pause == "hour") mark.until = QDateTime::currentDateTimeUtc().addSecs(3000);
        if (pause == "ended") mark.until = QDateTime::currentDateTimeUtc().addSecs(-60);
        writePause(pauseFilePath(fake.appState), mark);
    }
    const QString takeover = opt("--takeover", "none");
    const QByteArray answer =
        takeover == "offer" ? R"({"ok":true,"error":"","state":"found","found":["/home/someone/.config/systemd/user/drive-sync.service","/home/someone/.config/systemd/user/drive-sync.timer","/home/someone/.local/bin/drive-sync.sh","/home/someone/.config/rclone/gdrive-filtros.txt"],"blockers":[]})"
        : takeover == "running" ? R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running"]})"
        : takeover == "blocked" ? R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running","differentValue:Nice"]})"
        : takeover == "failed" ? R"({"ok":false,"error":"could not stop the old timers: Access denied","state":"","found":[],"blockers":[]})"
        : R"({"ok":true,"error":"","state":"none","found":[],"blockers":[]})";
    put(dir.filePath("answer.json"), answer);
    // The fake drivesync answers every child call with the takeover check,
    // and logs the calls, which are printed at the end.
    put(dir.filePath("drivesync"), QString("#!/bin/sh\necho \"$*\" >> '%2'\ncat '%1'\n").arg(dir.filePath("answer.json"), dir.filePath("calls.log")).toUtf8());
    QFile::setPermissions(dir.filePath("drivesync"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    fake.self = dir.filePath("drivesync");
    Backend backend(real ? BackendPaths::defaults() : fake);
    TakeoverSetup takeoverSetup(dir.filePath("drivesync"));
    takeoverSetup.check();

    // none: the widget is already on the bar, so no banner shows.
    const QString bar = opt("--bar", "none");
    const QByteArray list = bar == "offer" ? "[]" : R"([{"id":"drivesync","enabled":true}])";
    put(dir.filePath("list.json"), list);
    put(dir.filePath("omarchy"), QString("#!/bin/sh\ncase \"$*\" in\n  'plugin list --json') cat '%1' ;;\nesac\n")
                                     .arg(dir.filePath("list.json")).toUtf8());
    QFile::setPermissions(dir.filePath("omarchy"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir().mkpath(dir.filePath("shipped"));
    if (bar == "blocked")
        QDir().mkpath(dir.filePath("plugins/drivesync"));
    PluginSetup pluginSetup({dir.filePath("shipped"), dir.filePath("plugins"), dir.filePath("omarchy"), QString(), QString(), QString()});
    pluginSetup.check();

    // The bar clock lives in shell.json; without --clock the bar has none.
    const QString clock = opt("--clock", "");
    QJsonObject clockEntry{{"id", "omarchy.clock"}, {"format", clock}};
    QJsonObject layout;
    layout["center"] = QJsonArray{clockEntry};
    QJsonObject barConfig;
    barConfig["layout"] = layout;
    QJsonObject shell;
    shell["bar"] = barConfig;
    put(dir.filePath("shell.json"), clock.isEmpty() ? QByteArray("{}") : QJsonDocument(shell).toJson());
    ClockFormat clockFormat(ClockPaths{{dir.filePath("shell.json")}, QLocale::system()});
    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("clockFormat", &clockFormat);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("pluginSetup", &pluginSetup);
    engine.rootContext()->setContextProperty("takeoverSetup", &takeoverSetup);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    window->resize(width, height);
    QTest::qWait(1500);
    if (fix != "none" && fixLater) {
        writeTheFix();
        QTest::qWait(400);
    }

    for (const QChar k : keys) {
        if (k == 't') QTest::keyClick(window, Qt::Key_Tab);
        else if (k == 'h') QTest::keyClick(window, Qt::Key_Question, Qt::ShiftModifier);
        else if (k == 'd') QTest::keyClick(window, Qt::Key_Down);
        else if (k == 'e') QTest::keyClick(window, Qt::Key_Escape);
        else if (k == 's') QTest::keyClick(window, Qt::Key_Space);
        else if (k == 'p') QTest::keyClick(window, Qt::Key_P);
        else if (k == 'o') QTest::keyClick(window, Qt::Key_T);
        else if (k == 'f') QTest::keyClick(window, Qt::Key_F);
        else if (k == 'D') QTest::keyClick(window, Qt::Key_D);
        else if (k == 'b') QTest::keyClick(window, Qt::Key_Backspace);
        else if (k == 'c') QTest::keyClick(window, Qt::Key_C);
        else if (k == 'l') QTest::keyClick(window, Qt::Key_L);
        else if (k == 'x') QTest::keyClick(window, Qt::Key_X);
        else if (k == 'r') QTest::keyClick(window, Qt::Key_Return);
        else if (k == 'w') QTest::qWait(1100);
        else if (k.isDigit()) QTest::keyClick(window, Qt::Key(Qt::Key_0 + k.digitValue()));
        QTest::qWait(250);
    }
    QTest::qWait(400);
    window->grabWindow().save(out);
    printf("status=%s runs=%lld conflicts=%lld -> %s\n", qPrintable(backend.status()), (long long)backend.runs().size(),
           (long long)backend.conflicts().size(), qPrintable(out));
    QFile calls(dir.filePath("calls.log"));
    if (calls.open(QIODevice::ReadOnly))
        printf("drivesync calls:\n%s", calls.readAll().constData());
    return 0;
}
