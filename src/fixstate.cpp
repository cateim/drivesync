#include "fixstate.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>

namespace {

constexpr int kFixFileVersion = 1;

QDateTime fromUnix(const QJsonValue &value) {
    const qint64 seconds = value.toInteger();
    return seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC) : QDateTime();
}

qint64 toUnix(const QDateTime &when) { return when.isValid() ? when.toSecsSinceEpoch() : 0; }

bool knownStep(const QString &step) { return fixWaiting(step) || fixWorking(step) || step == "failed"; }

FixState unreadable(const QString &why) {
    FixState state;
    state.step = "failed";
    state.error = why;
    return state;
}

}  // namespace

QString fixStatePath(const QString &appState) { return appState + "/fix.json"; }

QString fixDryRunLogPath(const QString &appState) { return appState + "/fix-dry-run.log"; }

bool fixPresent(const QString &appState) { return QFile::exists(fixStatePath(appState)); }

FixState readFix(const QString &appState) {
    QFile in(fixStatePath(appState));
    if (!in.exists())
        return FixState();
    if (!in.open(QIODevice::ReadOnly))
        return unreadable(QString("cannot read %1: %2").arg(in.fileName(), in.errorString()));
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value("version").toInt() != kFixFileVersion)
        return unreadable(QString("%1 is not a fix file this DriveSync knows").arg(in.fileName()));
    FixState state;
    state.step = object.value("step").toString();
    if (!knownStep(state.step))
        return unreadable(QString("%1 has an unknown step").arg(in.fileName()));
    state.failedStep = object.value("failedStep").toString();
    state.started = fromUnix(object.value("started"));
    state.stepStarted = fromUnix(object.value("stepStarted"));
    state.lastReminder = fromUnix(object.value("lastReminder"));
    state.copies = object.value("copies").toInt();
    state.error = object.value("error").toString();
    for (const QJsonValue &value : object.value("questions").toArray()) {
        const QJsonObject q = value.toObject();
        FixQuestion question;
        question.path = q.value("path").toString();
        question.kind = q.value("kind").toString();
        question.cloudSize = q.value("cloudSize").toInteger(-1);
        question.notebookSize = q.value("notebookSize").toInteger(-1);
        question.cloudTime = q.value("cloudTime").toInteger();
        question.notebookTime = q.value("notebookTime").toInteger();
        question.answer = q.value("answer").toString();
        state.questions << question;
    }
    for (const QJsonValue &value : object.value("touched").toArray())
        state.touched << value.toString();
    return state;
}

bool writeFix(const QString &appState, const FixState &state) {
    if (!QDir().mkpath(appState))
        return false;
    QJsonArray questions;
    for (const FixQuestion &q : state.questions)
        questions.append(QJsonObject{{"path", q.path},
                                     {"kind", q.kind},
                                     {"cloudSize", q.cloudSize},
                                     {"notebookSize", q.notebookSize},
                                     {"cloudTime", q.cloudTime},
                                     {"notebookTime", q.notebookTime},
                                     {"answer", q.answer}});
    const QJsonObject object{{"version", kFixFileVersion},
                             {"step", state.step},
                             {"failedStep", state.failedStep},
                             {"started", toUnix(state.started)},
                             {"stepStarted", toUnix(state.stepStarted)},
                             {"lastReminder", toUnix(state.lastReminder)},
                             {"copies", state.copies},
                             {"error", state.error},
                             {"questions", questions},
                             {"touched", QJsonArray::fromStringList(state.touched)}};
    QSaveFile out(fixStatePath(appState));
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return out.commit();
}

bool clearFix(const QString &appState) {
    const QString log = fixDryRunLogPath(appState);
    const bool logGone = !QFile::exists(log) || QFile::remove(log);
    QFile::remove(appState + "/fix-answers.json");  // the window's hand-over, stale from now on
    const QString file = fixStatePath(appState);
    return (!QFile::exists(file) || QFile::remove(file)) && logGone;
}

bool fixWaiting(const QString &step) { return step == "confirm" || step == "questions"; }

bool fixWorking(const QString &step) { return step == "preparing" || step == "base" || step == "finishing"; }
