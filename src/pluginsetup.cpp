#include "pluginsetup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

PluginPaths PluginPaths::defaults() {
    return {"/usr/share/drive-sync/omarchy-plugin",
            QDir::homePath() + "/.config/omarchy/plugins",
            QStandardPaths::findExecutable("omarchy"),
            QStandardPaths::findExecutable("omarchy-shell"),
            QDir::homePath() + "/.local/state/drive-sync-app/bar-declined",
            "/usr/share/drivesync/omarchy-plugin"};
}

PluginSetup::PluginSetup(const PluginPaths &paths, QObject *parent) : QObject(parent), m_paths(paths) {}

PluginSetup::~PluginSetup() {
    // An omarchy call still running at quit is stopped without its callback,
    // which would touch this half-destroyed object.
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect();
        process->kill();
        process->waitForFinished(500);
    }
}

void PluginSetup::setState(const QString &state, const QString &detail) {
    if (state == m_state && detail == m_detail)
        return;
    m_state = state;
    m_detail = detail;
    emit changed();
}

void PluginSetup::check() {
    if (m_paths.omarchy.isEmpty()) {
        setState("noOmarchy");
        return;
    }
    if (!QFileInfo(m_paths.shipped).isDir()) {
        setState("notShipped", m_paths.shipped);
        return;
    }
    if (!m_paths.declined.isEmpty() && QFileInfo::exists(m_paths.declined)) {
        setState("declined");
        return;
    }

    const QString link = m_paths.userPlugins + "/" + kPluginId;
    const QString target = QFileInfo(m_paths.shipped).absoluteFilePath();
    const QFileInfo info(link);
    // isSymLink first: exists() follows the link and says false for a dangling one.
    if (info.isSymLink()) {
        if (info.symLinkTarget() != target) {
            setState("blocked", link);
            return;
        }
        readList();
        return;
    }
    if (info.exists()) {
        setState("blocked", link);
        return;
    }
    if (!QDir().mkpath(m_paths.userPlugins) || !QFile::link(target, link)) {
        setState("failed", link);
        return;
    }
    // The shell watches its plugin folder, but asking it to look again makes
    // sure the new widget is known before anyone tries to enable it.
    setState("checking");
    run(m_paths.omarchyShell, {"shell", "rescanPlugins"},
        [this](bool, const QByteArray &, const QByteArray &) { readList(); });
}

void PluginSetup::readList() {
    setState("checking");
    run(m_paths.omarchy, {"plugin", "list", "--json"}, [this](bool ok, const QByteArray &output, const QByteArray &error) {
        if (!ok) {
            setState(m_asked ? "failed" : "unchecked", QString::fromUtf8(error).trimmed());
            return;
        }
        bool enabled = false;
        for (const QJsonValue &value : QJsonDocument::fromJson(output).array()) {
            const QJsonObject plugin = value.toObject();
            if (!plugin.value("enabled").toBool())
                continue;
            enabled = enabled || plugin.value("id").toString() == kPluginId;
        }
        if (!enabled && legacyLinked()) {
            replaceLegacy();
            return;
        }
        setState(enabled ? "enabled" : "offer");
    });
}

void PluginSetup::accept() {
    if (m_state != "offer")
        return;
    m_asked = true;
    setState("checking");
    run(m_paths.omarchy, {"plugin", "enable", kPluginId}, [this](bool ok, const QByteArray &, const QByteArray &error) {
        if (ok)
            readList();
        else
            setState("failed", QString::fromUtf8(error).trimmed());
    });
}

// The link the drivesync package made, now dangling: pacman removed
// /usr/share/drivesync with the old package, so the shell no longer lists that
// widget.
bool PluginSetup::legacyLinked() const {
    if (m_legacyTried || m_paths.legacyShipped.isEmpty())
        return false;
    const QFileInfo link(m_paths.userPlugins + "/" + kLegacyPluginId);
    return link.isSymLink() && link.symLinkTarget() == m_paths.legacyShipped;
}

// The user had put the drivesync widget on the bar, and drive-sync's is the
// same widget renamed, so it takes the old one's place without asking again. The
// old entry goes only after the new one is in, and its link with it. An
// enable that fails means the old widget was not on the bar: nothing to
// replace, and the usual offer follows.
void PluginSetup::replaceLegacy() {
    m_legacyTried = true;
    const QString link = m_paths.userPlugins + "/" + kLegacyPluginId;
    setState("checking");
    run(m_paths.omarchy, {"plugin", "enable", kPluginId, "--after", kLegacyPluginId},
        [this, link](bool ok, const QByteArray &, const QByteArray &) {
            if (!ok) {
                QFile::remove(link);
                readList();
                return;
            }
            run(m_paths.omarchy, {"plugin", "disable", kLegacyPluginId},
                [this, link](bool ok, const QByteArray &, const QByteArray &error) {
                    QFile::remove(link);
                    if (ok)
                        readList();
                    else
                        setState("failed", QString::fromUtf8(error).trimmed());
                });
        });
}

void PluginSetup::decline() {
    // "blocked" too: someone else's folder or link at the path is there for
    // good, and its notice would otherwise show at every start.
    if (m_state != "offer" && m_state != "blocked")
        return;
    if (!m_paths.declined.isEmpty() && QDir().mkpath(QFileInfo(m_paths.declined).absolutePath())) {
        QFile mark(m_paths.declined);
        if (mark.open(QIODevice::WriteOnly))
            mark.write("The bar widget was declined in the DriveSync window. Delete this file to be asked again.\n");
    }
    setState("declined");
}

void PluginSetup::run(const QString &program, const QStringList &arguments,
                      std::function<void(bool, const QByteArray &, const QByteArray &)> done) {
    if (program.isEmpty()) {
        done(false, {}, {});
        return;
    }
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0, process->readAllStandardOutput(),
             process->readAllStandardError());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        done(false, {}, process->errorString().toUtf8());
        process->deleteLater();
    });
    // omarchy answers in well under a second; a hung call must not leave the
    // banner spinning forever.
    QTimer::singleShot(15 * 1000, process, [process] { process->kill(); });
    process->start(program, arguments);
}
