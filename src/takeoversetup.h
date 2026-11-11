#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

// The window's side of a takeover. It runs "drivesync takeover" as a child
// process, so a slow systemctl or the wait for a run to end never blocks the
// window, and keeps what the window shows.
class TakeoverSetup : public QObject {
    Q_OBJECT
    // "checking" | "none" | "managed" | "offer" | "blocked" | "working" | "failed"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QStringList found READ found NOTIFY changed)
    Q_PROPERTY(QStringList blockers READ blockers NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    // Confirmed, but a run started between the offer and the confirmation:
    // the takeover happens by itself at the first check that offers it again.
    Q_PROPERTY(bool pending READ pending NOTIFY changed)

public:
    explicit TakeoverSetup(const QString &program, QObject *parent = nullptr);
    ~TakeoverSetup() override;

    QString state() const { return m_state; }
    QStringList found() const { return m_found; }
    QStringList blockers() const { return m_blockers; }
    QString detail() const { return m_detail; }
    bool pending() const { return m_pending; }

    // Asks what a takeover would do. A blocked one is asked again every
    // minute, so the offer shows up once the run that blocked it ends.
    Q_INVOKABLE void check();
    // Takes over; only after check() offered it.
    Q_INVOKABLE void accept();
    // Drops a confirmation that waits for a run to end.
    Q_INVOKABLE void cancelPending();

signals:
    void changed();

private:
    void run(const QStringList &arguments);
    void apply(const QByteArray &output, bool applying);

    QString m_program;
    QString m_state = "checking";
    QStringList m_found;
    QStringList m_blockers;
    QString m_detail;
    bool m_pending = false;
    QTimer m_recheck;
};
