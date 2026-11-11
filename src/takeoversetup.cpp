#include "takeoversetup.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace {

QStringList strings(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

}  // namespace

TakeoverSetup::TakeoverSetup(const QString &program, QObject *parent) : QObject(parent), m_program(program) {
    m_recheck.setInterval(60 * 1000);
    connect(&m_recheck, &QTimer::timeout, this, [this] {
        if (m_state == "blocked")
            check();
    });
    m_recheck.start();
}

TakeoverSetup::~TakeoverSetup() {
    // A takeover still running at quit keeps going in its own process; only
    // the callback into this half-destroyed object is cut.
    for (QProcess *process : findChildren<QProcess *>())
        process->disconnect();
}

void TakeoverSetup::check() {
    if (m_state == "working")
        return;
    run({"takeover", "--check", "--json"});
}

void TakeoverSetup::accept() {
    if (m_state != "offer")
        return;
    m_state = "working";
    emit changed();
    run({"takeover", "--apply", "--json"});
}

void TakeoverSetup::cancelPending() {
    if (!m_pending)
        return;
    m_pending = false;
    emit changed();
}

void TakeoverSetup::run(const QStringList &arguments) {
    auto *process = new QProcess(this);
    const bool applying = arguments.contains("--apply");
    connect(process, &QProcess::finished, this, [this, process, applying] {
        apply(process->readAllStandardOutput(), applying);
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, applying](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        apply({}, applying);
        process->deleteLater();
    });
    process->start(m_program, arguments);
}

void TakeoverSetup::apply(const QByteArray &output, bool applying) {
    const QJsonDocument document = QJsonDocument::fromJson(output);
    const QJsonObject answer = document.object();
    if (!document.isObject() || (!answer.value("ok").toBool() && !applying)) {
        m_state = "failed";
        m_detail = answer.value("error").toString();
        if (m_detail.isEmpty())
            m_detail = tr("DriveSync could not read the drive-sync setup.");
        emit changed();
        return;
    }
    if (applying) {
        const QStringList blockers = strings(answer.value("blockers"));
        if (!answer.value("ok").toBool() && blockers == QStringList{"running"}) {
            // Runs take about half of each tick, so one often starts while
            // the dialog is open. Nothing moved: wait for it, then go on.
            m_state = "blocked";
            m_blockers = blockers;
            m_pending = true;
            emit changed();
            return;
        }
        if (!answer.value("ok").toBool()) {
            m_pending = false;
            m_state = "failed";
            m_detail = answer.value("error").toString();
            emit changed();
            return;
        }
        // Taken over: look again, which now says "managed".
        m_state = "checking";
        emit changed();
        check();
        return;
    }
    const QString state = answer.value("state").toString();
    m_found = strings(answer.value("found"));
    m_blockers = strings(answer.value("blockers"));
    m_detail.clear();
    if (state == "found")
        m_state = m_blockers.isEmpty() ? "offer" : "blocked";
    else if (state == "none" || state == "managed")
        m_state = state;
    else
        m_state = "failed";
    // Anything but a run in the way needs the user to look again.
    if (m_state != "offer" && m_blockers != QStringList{"running"})
        m_pending = false;
    if (m_pending && m_state == "offer") {
        m_pending = false;
        accept();
        return;
    }
    emit changed();
}
