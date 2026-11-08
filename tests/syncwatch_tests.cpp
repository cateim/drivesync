#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include "backend.h"
#include "conflictcache.h"
#include "pluginsetup.h"
#include "snapshot.h"
#include "statuscommand.h"
#include "synclog.h"
#include "theme.h"
#include "translator.h"

namespace {

// One line the way rclone writes it: "2026/11/07 15:28:19 INFO  : message".
QString line(const QDateTime &when, const QString &level, const QString &message) {
    return QString("%1 %2: %3\n").arg(when.toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), message);
}

QString change(const QString &tag, const QString &what, const QString &path) {
    return QString("- %1%2 - %3").arg(tag.leftJustified(18), what.leftJustified(43), path);
}

// A normal run that copied one file each way, as drive-sync logs it.
QString successfulRun(const QDateTime &start, int seconds) {
    QString log;
    log += line(start, "INFO", "Bisyncing with Comparison Settings: ");
    log += "{\n\t\"Modtime\": true,\n}\n";
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "INFO", "Building Path1 and Path2 listings");
    log += line(start.addSecs(60), "INFO", "");
    log += "Transferred:   \t          0 B / 0 B, -, 0 B/s, ETA -\nChecks:             14560 / 14560, 100%, Listed 16680\nElapsed time:        59.9s\n\n";
    log += line(start.addSecs(seconds - 20), "INFO", "Path1 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path1", "File changed: size (larger), time (newer)", "Notes/todo.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path1:    1 changes:    0 new,    1 modified,    0 deleted");
    log += line(start.addSecs(seconds - 20), "INFO", "Path2 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path2", "File is new", "GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path2:    1 changes:    1 new,    0 modified,    0 deleted");
    log += line(start.addSecs(seconds - 19), "INFO", "Applying changes");
    log += line(start.addSecs(seconds - 19), "INFO", change("Path2", "Queue copy to Path1", "gdrive{x}:/GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 2), "INFO", "Updating listings");
    log += line(start.addSecs(seconds), "INFO", "Bisync successful");
    return log;
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

// A systemctl that answers like systemd with the service idle and the timer on.
QString fakeSystemctl(const QString &dir, const QDateTime &serviceStart, const QString &serviceActive = "inactive") {
    const QString path = dir + "/systemctl";
    const QString script = QString(
        "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *list-timers*) echo '[{\"next\":%1,\"unit\":\"drive-sync.timer\"}]' ;;\n"
        "  *) printf 'LoadState=loaded\\nActiveState=%2\\nUnitFileState=static\\nResult=success\\n"
        "ExecMainStartTimestamp=@%3\\n\\nLoadState=loaded\\nActiveState=active\\nUnitFileState=enabled\\nResult=success\\n' ;;\n"
        "esac\n")
        .arg(QDateTime::currentDateTime().addSecs(420).toMSecsSinceEpoch() * 1000)
        .arg(serviceActive)
        .arg(serviceStart.toSecsSinceEpoch());
    write(path, script.toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

SyncInputs healthy() {
    SyncInputs in;
    in.serviceLoad = "loaded";
    in.serviceActive = "inactive";
    in.serviceResult = "success";
    in.timerActive = "active";
    in.timerFileState = "enabled";
    in.now = QDateTime::currentDateTime();
    in.serviceStart = in.now.addSecs(-600);
    in.lastStart = in.serviceStart;
    in.lastOutcome = "success";
    in.lastSuccess = in.now.addSecs(-120);
    return in;
}

// A run that stopped on lost listings, as drive-sync logs it.
QString failedRun(const QDateTime &start) {
    QString log;
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
    log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
    log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");
    return log;
}

// A fake omarchy and omarchy-shell that append every call to calls.log, and
// answer "plugin list --json" with list.json, all inside dir.
PluginPaths fakeOmarchy(const QString &dir, const QByteArray &list) {
    write(dir + "/list.json", list);
    const QString omarchy = QString(
        "#!/bin/sh\n"
        "echo \"$*\" >> '%1/calls.log'\n"
        "case \"$*\" in\n"
        "  'plugin list --json') cat '%1/list.json' ;;\n"
        "esac\n").arg(dir);
    write(dir + "/omarchy", omarchy.toUtf8());
    write(dir + "/omarchy-shell", QString("#!/bin/sh\necho \"shell-cmd $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    for (const QString &name : {QString("omarchy"), QString("omarchy-shell")})
        QFile::setPermissions(dir + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir(dir).mkpath("shipped");
    return {dir + "/shipped", dir + "/plugins", dir + "/omarchy", dir + "/omarchy-shell"};
}

QStringList calls(const QString &dir) {
    QFile file(dir + "/calls.log");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

// What buildSnapshot sees with the service idle, the timer on and enabled,
// and the log already parsed into runs.
SnapshotSources idleSources(const QString &log, const QDateTime &now) {
    SnapshotSources src;
    src.runs = SyncLog::parse(log);
    src.logFound = true;
    src.service = {{"LoadState", "loaded"}, {"ActiveState", "inactive"}, {"Result", "success"},
                   {"ExecMainStartTimestamp", QString("@%1").arg(now.addSecs(-100).toSecsSinceEpoch())}};
    src.timer = {{"ActiveState", "active"}, {"UnitFileState", "enabled"}};
    src.now = now;
    return src;
}

}  // namespace

class SyncwatchTests : public QObject {
    Q_OBJECT

private slots:
    void parsesASuccessfulRun()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(15, 28, 19));
        const auto runs = SyncLog::parse(successfulRun(start, 485));
        QCOMPARE(runs.size(), 1);
        const SyncRun &run = runs.first();
        QCOMPARE(run.outcome, QString("success"));
        QCOMPARE(run.start, start);
        QCOMPARE(run.start.secsTo(run.end), 485);
        QCOMPARE(run.cloud.modified, 1);
        QCOMPARE(run.notebook.added, 1);
        QCOMPARE(run.listed, 16680LL);
        QVERIFY(!run.resync);
        QCOMPARE(run.changes.size(), 2);  // The "Queue copy" line repeats a change.
        QCOMPARE(run.changes.at(0).side, QString("cloud"));
        QCOMPARE(run.changes.at(0).kind, QString("modified"));
        QCOMPARE(run.changes.at(0).path, QString("Notes/todo.md"));
        QCOMPARE(run.changes.at(1).side, QString("notebook"));
        QCOMPARE(run.changes.at(1).kind, QString("new"));
        QVERIFY(run.errors.isEmpty());
    }

    void readsTheCauseOfAFailedRun()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 48, 47));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
        log += "Tip: here are the filenames we were looking for. Do they exist? \n";
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");

        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);  // The trailing lines belong to the same run.
        QCOMPARE(runs.first().outcome, QString("failed"));
        QCOMPARE(runs.first().causeKind, QString("lostListings"));
        QCOMPARE(runs.first().errors.size(), 3);
    }

    void namesTheFirstRealErrorAsTheCause()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 10, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(30), "ERROR", "-                   Size not equal in listing. Path1: 5972, Path2: 7617 - GitHub/app/x.yml");
        log += line(start.addSecs(30), "ERROR", "Bisync aborted. Must run --resync to recover.");
        const auto outOfSync = SyncLog::parse(log);
        QCOMPARE(outOfSync.first().causeKind, QString("error"));
        QVERIFY(outOfSync.first().causeDetail.startsWith("Size not equal in listing"));

        log += line(start.addSecs(31), "ERROR", "Bisync critical error: path1 and path2 are out of sync, run --resync to recover");
        QCOMPARE(SyncLog::parse(log).first().causeKind, QString("outOfSync"));
    }

    void countsAnErrorBeforeAnyRunAsAFailedRun()
    {
        const QDateTime when(QDate(2026, 11, 7), QTime(9, 0, 0));
        QString log = successfulRun(when.addSecs(-3600), 480);
        log += line(when, "CRITICAL", "Failed to create file system for \"gdrive:\": couldn't find root directory ID");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("success"));
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("error"));
        QVERIFY(runs.last().causeDetail.contains("couldn't find root directory ID"));
    }

    void marksCutOffAndOpenRuns()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(60), "INFO", "Path1 checking for diffs");
        log += line(start.addSecs(900), "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(960), "INFO", "Applying changes");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("interrupted"));
        QCOMPARE(runs.last().outcome, QString("open"));
        QCOMPARE(runs.last().phase, QString("applying"));
        QCOMPARE(runs.last().lastSeen, start.addSecs(960));
    }

    void skipsARunWhoseStartWasCutOff()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += successfulRun(start.addSecs(600), 480);
        QCOMPARE(SyncLog::parse(log, 40, true).size(), 1);
        QCOMPARE(SyncLog::parse(log, 40, false).size(), 2);
    }

    void spotsAResyncAndCapsItsChanges()
    {
        const QDateTime start(QDate(2026, 11, 2), QTime(9, 5, 16));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", "Copying Path2 files to Path1");
        for (int i = 0; i < 305; ++i)
            log += line(start, "INFO", change("Path2", "File is new", QString("f%1.txt").arg(i)));
        log += line(start.addSecs(9000), "INFO", "Resync updating listings");
        log += line(start.addSecs(9002), "INFO", "Bisync successful");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);
        QVERIFY(runs.first().resync);
        QCOMPARE(runs.first().changes.size(), 300);
        QCOMPARE(runs.first().changesOmitted, 5);
    }

    void keepsTheNewestRuns()
    {
        QString log;
        const QDateTime start(QDate(2026, 11, 7), QTime(0, 0, 0));
        for (int i = 0; i < 5; ++i)
            log += successfulRun(start.addSecs(i * 900), 480);
        const auto runs = SyncLog::parse(log, 3);
        QCOMPARE(runs.size(), 3);
        QCOMPARE(runs.last().start, start.addSecs(4 * 900));
    }

    void readsTheTailFromAWholeLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("drive-sync.log");
        QVERIFY(write(path, "first line\nsecond line\nthird line\n"));
        bool found = false, truncated = false;
        QCOMPARE(SyncLog::readTail(path, 20, &found, &truncated), QString("third line\n"));
        QVERIFY(found);
        QVERIFY(truncated);
        QCOMPARE(SyncLog::readTail(path, 1000, &found, &truncated).count('\n'), 3);
        QVERIFY(!truncated);
        SyncLog::readTail(dir.filePath("missing.log"), 1000, &found);
        QVERIFY(!found);
    }

    void summarizesAHealthySync()
    {
        const auto s = Backend::summarize(healthy());
        QCOMPARE(s.status, QString("ok"));
        QVERIFY(s.warnings.isEmpty());
    }

    void putsTheWorstProblemFirst()
    {
        SyncInputs in = healthy();
        in.lastOutcome = "failed";
        in.lastCauseKind = "lostListings";
        in.conflicts = 2;
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("lostListings"));
        QCOMPARE(s.warnings, QStringList{"conflicts"});

        in.running = true;
        QCOMPARE(Backend::summarize(in).status, QString("running"));

        in = healthy();
        in.conflicts = 1;
        in.timerFileState = "disabled";
        s = Backend::summarize(in);
        QCOMPARE(s.status, QString("conflicts"));
        QCOMPARE(s.warnings, QStringList{"timerDisabled"});

        in = healthy();
        in.serviceLoad = "not-found";
        QCOMPARE(Backend::summarize(in).status, QString("notLoaded"));
    }

    void trustsSystemdOnlyForItsNewestRun()
    {
        SyncInputs in = healthy();
        in.serviceResult = "timeout";
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("timeout"));

        // A resync from drive-sync.sh ran after the service failed, and worked.
        in.lastStart = in.serviceStart.addSecs(3600);
        QCOMPARE(Backend::summarize(in).status, QString("ok"));
    }

    void tellsATimerLeftOffFromAResync()
    {
        SyncInputs in = healthy();
        in.timerActive = "inactive";
        QCOMPARE(Backend::summarize(in).status, QString("timerOff"));

        in.resyncRunning = true;
        const auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("resync"));
        QVERIFY(s.warnings.isEmpty());

        in.resyncRunning = false;
        in.lastOutcome = "failed";
        QCOMPARE(Backend::summarize(in).warnings, QStringList{"timerOff"});
    }

    void noticesRunsThatStopped()
    {
        SyncInputs in = healthy();
        in.lastSuccess = in.now.addSecs(-3 * 3600);
        QCOMPARE(Backend::summarize(in).status, QString("stale"));
        in.lastOutcome = "interrupted";
        QCOMPARE(Backend::summarize(in).status, QString("interrupted"));
    }

    void parsesSystemctl()
    {
        const auto units = Backend::parseShow("LoadState=loaded\nActiveState=inactive\nExecMainStartTimestamp=@1794076098\n\n"
                                              "LoadState=loaded\nActiveState=active\n");
        QCOMPARE(units.size(), 2);
        QCOMPARE(units.at(0).value("ExecMainStartTimestamp"), QString("@1794076098"));
        QCOMPARE(units.at(1).value("ActiveState"), QString("active"));

        const QDateTime next = Backend::parseNextElapse(R"([{"next":1794076998808087,"unit":"drive-sync.timer"}])");
        QCOMPARE(next.toMSecsSinceEpoch(), 1794076998808LL);
        QVERIFY(!Backend::parseNextElapse("[]").isValid());
        QVERIFY(!Backend::parseNextElapse(R"([{"next":0}])").isValid());
    }

    void checksThePidInTheResyncMark()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("state");
        QDir(dir.path()).mkpath("proc/4242");
        QDir(dir.path()).mkpath("proc/4343");
        const QString state = dir.filePath("state"), proc = dir.filePath("proc");
        QVERIFY(!Backend::resyncRunning(state, proc));

        QVERIFY(write(dir.filePath("proc/4242/cmdline"), QByteArray("bash\0/home/someone/.local/bin/drive-sync.sh\0resync\0", 52)));
        QVERIFY(write(dir.filePath("proc/4343/cmdline"), QByteArray("vim\0notes.md\0", 13)));
        QVERIFY(write(state + "/pausado", "4242\n"));
        QVERIFY(Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "4343\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "9999\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
    }

    void watchesARealFolder()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive/Notes");
        QDir(dir.path()).mkpath("Drive/.git");
        QDir(dir.path()).mkpath("state");
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md.conflict1"), "older"));
        QVERIFY(write(dir.filePath("Drive/.git/index.conflict1"), "never synced"));
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md"), "newer"));
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));

        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find")};
        paths.appState = dir.filePath("app");
        Backend backend(paths);
        QTRY_COMPARE(backend.conflicts().size(), 1);
        // The scan leaves its count for the status command and the bar.
        QTRY_COMPARE(readConflictCache(paths.appState + "/conflicts.json").count, 1);
        QCOMPARE(backend.conflicts().first().toMap().value("path").toString(), QString("Notes/todo.md.conflict1"));
        QCOMPARE(backend.conflicts().first().toMap().value("folder").toString(), QString("Notes"));
        QTRY_COMPARE(backend.status(), QString("conflicts"));
        QCOMPARE(backend.runs().size(), 1);
        QCOMPARE(backend.typicalSeconds(), 480);
        QVERIFY(backend.nextRun() > QDateTime::currentDateTime());
        QVERIFY(backend.currentRun().isEmpty());

        // A run starts: the log knows before the systemd poll does.
        QFile log(paths.log);
        QVERIFY(log.open(QIODevice::Append));
        log.write(line(QDateTime::currentDateTime(), "INFO",
                       R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")").toUtf8());
        log.close();
        QTRY_COMPARE(backend.status(), QString("running"));
        QCOMPARE(backend.currentRun().value("phase").toString(), QString("listing"));
        QCOMPARE(backend.runs().size(), 1);
    }

    // The window colors itself from the same tone the bar reads.
    void backendExposesTone()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                                 QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QCOMPARE(backend.tone(), QString("ok"));
    }

    void reportsAMissingDriveAndLog()
    {
        QTemporaryDir dir;
        BackendPaths paths{dir.filePath("missing.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), QDateTime::currentDateTime()),
                           QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("noLog"));
        QCOMPARE(backend.conflictError(), QString("noDrive"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Runs").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Runs": "Execuções", "Quit": "Sair"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Runs": "Rodadas"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Runs"), QString("Rodadas"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Sair"));
        QVERIFY(translator.translate("Main", "Space").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // The shipped catalogs: valid JSON, and every plural string of the
    // Portuguese one has the forms pt-BR uses.
    void shippedCatalogsParse()
    {
        for (const QString &code : {QString("en"), QString("pt-BR")}) {
            QFile file(QString(SRCDIR) + "/../src/i18n/" + code + ".json");
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            QJsonParseError error;
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll(), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(code + ": " + error.errorString()));
            QVERIFY(!catalog.isEmpty());
            for (auto it = catalog.begin(); it != catalog.end(); ++it)
                if (it.value().isObject() && code == "pt-BR")
                    QVERIFY2(it.value().toObject().contains("one") && it.value().toObject().contains("other"),
                             qPrintable(it.key()));
        }
    }

    // The tone groups every headline the way the window colors it.
    void toneGroupsEveryStatus()
    {
        for (const char *s : {"failed", "timerOff", "unavailable", "notLoaded", "noLog", "noDrive"})
            QCOMPARE(toneFor(s), QString("error"));
        for (const char *s : {"interrupted", "skipped", "stale", "conflicts"})
            QCOMPARE(toneFor(s), QString("warning"));
        QCOMPARE(toneFor("running"), QString("busy"));
        QCOMPARE(toneFor("resync"), QString("busy"));
        QCOMPARE(toneFor("ok"), QString("ok"));
    }

    // Two failures after a success: the problem started with the first one.
    void snapshotFailingSinceIsFirstFailureAfterSuccess()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const QString log = successfulRun(t0, 480) + failedRun(t0.addSecs(900)) + failedRun(t0.addSecs(1800));
        const SnapshotSources src = idleSources(log, t0.addSecs(2000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("failed"));
        QCOMPARE(s.failingSince, src.runs.at(1).start);
    }

    void snapshotNoFailingSinceWhenOk()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const Snapshot s = buildSnapshot(idleSources(successfulRun(t0, 480), t0.addSecs(600)));
        QCOMPARE(s.summary.status, QString("ok"));
        QCOMPARE(s.tone, QString("ok"));
        QVERIFY(!s.failingSince.isValid());
    }

    // A manual run only makes sense with the unit loaded and nothing going.
    void snapshotCanSyncNowOnlyWhenIdle()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources(successfulRun(t0, 480), t0.addSecs(600));
        QVERIFY(buildSnapshot(src).canSyncNow);

        SnapshotSources busy = src;
        busy.service["ActiveState"] = "activating";
        QVERIFY(!buildSnapshot(busy).canSyncNow);

        SnapshotSources resync = src;
        resync.resyncRunning = true;
        QVERIFY(!buildSnapshot(resync).canSyncNow);

        SnapshotSources noSystemctl = src;
        noSystemctl.systemctlWorks = false;
        QVERIFY(!buildSnapshot(noSystemctl).canSyncNow);

        SnapshotSources notLoaded = src;
        notLoaded.service["LoadState"] = "not-found";
        QVERIFY(!buildSnapshot(notLoaded).canSyncNow);
    }

    void snapshotLastRunIsNewestFinished()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const SnapshotSources src = idleSources(successfulRun(t0, 480) + failedRun(t0.addSecs(900)), t0.addSecs(1000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.lastRunStart, src.runs.at(1).start);
        QCOMPARE(s.lastRunEnd, src.runs.at(1).end);
        QCOMPARE(s.lastRunOutcome, QString("failed"));
        QCOMPARE(s.lastSuccess, src.runs.at(0).end);
    }

    // Version 1 of what "syncwatch status --json" prints: every key always
    // there, times as Unix seconds, 0 when unknown.
    void statusJsonContract()
    {
        Snapshot s;
        s.summary.status = "failed";
        s.summary.causeKind = "outOfSync";
        s.summary.warnings = {"timerDisabled"};
        s.tone = "error";
        s.lastSuccess = QDateTime(QDate(2026, 11, 7), QTime(15, 0, 0), QTimeZone::UTC);
        s.failingSince = QDateTime(QDate(2026, 11, 7), QTime(15, 30, 0), QTimeZone::UTC);
        s.conflicts = 2;
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        const QJsonObject json = snapshotToJson(s, QDateTime(), now);
        QCOMPARE(json.value("version").toInt(), 1);
        QCOMPARE(json.value("now").toInteger(), 1794067200);
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("tone").toString(), QString("error"));
        QCOMPARE(json.value("lastSuccess").toInteger(), 1794063600);
        QCOMPARE(json.value("failingSince").toInteger(), 1794065400);
        QCOMPARE(json.value("nextRun").toInteger(), 0);
        QCOMPARE(json.value("warnings").toArray(), QJsonArray{"timerDisabled"});
        QCOMPARE(json.value("conflicts").toInt(), 2);
        QVERIFY(json.value("lastRun").toObject().value("outcome").isString());
        QCOMPARE(json.value("lastRun").toObject().value("start").toInteger(), 0);
        QVERIFY(json.value("currentRun").toObject().value("phase").isString());
        QCOMPARE(json.keys().size(), 17);
    }

    void conflictCacheRoundTrip()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("state/conflicts.json");  // the folder does not exist yet
        ConflictCount c;
        c.count = 3;
        c.scannedAt = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QVERIFY(writeConflictCache(file, c));
        const ConflictCount back = readConflictCache(file);
        QCOMPARE(back.count, 3);
        QCOMPARE(back.error, QString());
        QCOMPARE(back.scannedAt, c.scannedAt);
        QVERIFY(conflictCacheFresh(back, c.scannedAt.addSecs(60)));
        QVERIFY(!conflictCacheFresh(back, c.scannedAt.addSecs(kConflictCacheSeconds)));
    }

    void conflictCacheCorruptIsStale()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("conflicts.json");
        QVERIFY(write(file, "not json"));
        const ConflictCount c = readConflictCache(file);
        QVERIFY(!c.scannedAt.isValid());
        QVERIFY(!conflictCacheFresh(c, QDateTime::currentDateTimeUtc()));
    }

    // A clock that moved back must not keep an old count alive for hours.
    void conflictCacheFromTheFutureIsStale()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        ConflictCount c;
        c.scannedAt = now.addSecs(600);
        QVERIFY(!conflictCacheFresh(c, now));
    }

    void statusCommandJson()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                                 QStandardPaths::findExecutable("find"), dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("version").toInt(), 1);
        QCOMPARE(json.value("status").toString(), QString("ok"));
        QVERIFY(json.value("nextRun").toInteger() > QDateTime::currentSecsSinceEpoch());
        QVERIFY(err.isEmpty());
        // The scan it had to make is left for the next call.
        QVERIFY(conflictCacheFresh(readConflictCache(paths.appState + "/conflicts.json"), QDateTime::currentDateTimeUtc()));
    }

    // A long outage: the last success is older than the 40 runs the window
    // lists, and the state must still say when the trouble began.
    void statusKeepsTheStartOfALongOutage()
    {
        QTemporaryDir dir;
        const QDateTime t0 = QDateTime::currentDateTime().addSecs(-46 * 900 - 600);
        QString log = successfulRun(t0, 480);
        for (int i = 1; i <= 45; ++i)
            log += failedRun(t0.addSecs(i * 900));
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), t0.addSecs(45 * 900)),
                                 QStandardPaths::findExecutable("find"), dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("failingSince").toInteger(), t0.addSecs(900).toSecsSinceEpoch());
        QCOMPARE(json.value("lastSuccess").toInteger(), t0.addSecs(480).toSecsSinceEpoch());
    }

    void statusCommandRejectsUnknownFlag()
    {
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--jsno"}, BackendPaths{}, o, e), 1);
        QVERIFY(err.contains("usage: syncwatch"));
        QVERIFY(out.isEmpty());
    }

    // A stuck systemctl must not hang the bar: the answer is "unavailable".
    void statusCommandSystemctlTimeout()
    {
        QTemporaryDir dir;
        const QString systemctl = dir.filePath("systemctl");
        QVERIFY(write(systemctl, "#!/bin/sh\nsleep 5\n"));
        QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), systemctl, QStandardPaths::findExecutable("find"),
                                 dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(runStatusCommand({"--json"}, paths, o, e, 200), 0);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("status").toString(), QString("unavailable"));
    }

    void pluginSetupLinksWhenAbsent()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        const QFileInfo link(paths.userPlugins + "/syncwatch");
        QVERIFY(link.isSymLink());
        QCOMPARE(link.symLinkTarget(), QFileInfo(paths.shipped).absoluteFilePath());
        QVERIFY(calls(dir.path()).contains("shell-cmd shell rescanPlugins"));
    }

    // A folder the user made with the same name is theirs: never touched.
    void pluginSetupLeavesForeignFolder()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        QDir(paths.userPlugins).mkpath("syncwatch");
        QVERIFY(write(paths.userPlugins + "/syncwatch/mine.txt", "keep"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
        QFile mine(paths.userPlugins + "/syncwatch/mine.txt");
        QVERIFY(mine.open(QIODevice::ReadOnly));
        QCOMPARE(mine.readAll(), QByteArray("keep"));
        QVERIFY(calls(dir.path()).isEmpty());
    }

    void pluginSetupOffersReplaceForOldWidget()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), R"([{"id":"cateim.drive-sync","enabled":true}])");
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.shipped, paths.userPlugins + "/syncwatch"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("replaceOffer"));
        QCOMPARE(calls(dir.path()), QStringList{"plugin list --json"});  // the link was there: no rescan
    }

    void pluginSetupAcceptReplaces()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), R"([{"id":"cateim.drive-sync","enabled":true}])");
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.shipped, paths.userPlugins + "/syncwatch"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("replaceOffer"));

        QFile::remove(dir.filePath("calls.log"));
        QVERIFY(write(dir.filePath("list.json"),
                      R"([{"id":"syncwatch","enabled":true},{"id":"cateim.drive-sync","enabled":false}])"));
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("enabled"));
        QCOMPARE(calls(dir.path()), (QStringList{"plugin enable syncwatch --after cateim.drive-sync",
                                                 "plugin disable cateim.drive-sync", "plugin list --json"}));
    }

    void pluginSetupEnabled()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), R"([{"id":"syncwatch","enabled":true}])");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("enabled"));
    }

    // "No" is remembered: no offer, and no link that would undo a removal.
    void pluginSetupRemembersNo()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.declined = dir.filePath("app/bar-declined");
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("offer"));
            setup.decline();
            QCOMPARE(setup.state(), QString("declined"));
        }
        QFile::remove(paths.userPlugins + "/syncwatch");  // as `omarchy plugin remove` would
        QFile::remove(dir.filePath("calls.log"));
        PluginSetup again(paths);
        again.check();
        QCOMPARE(again.state(), QString("declined"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
        QVERIFY(calls(dir.path()).isEmpty());
    }

    void pluginSetupWithoutOmarchy()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.omarchy.clear();
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("noOmarchy"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/syncwatch").isSymLink());
    }
};

QTEST_MAIN(SyncwatchTests)
#include "syncwatch_tests.moc"
