#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

// Puts SyncWatch's bar widget on the Omarchy bar. The package installs the
// widget under /usr/share, but Omarchy loads third-party plugins only from
// ~/.config/omarchy/plugins, so the window links it there and, when the
// user agrees, enables it through Omarchy's own `omarchy plugin` commands.
// Omarchy's shell.json is never edited here.

struct PluginPaths {
    QString shipped;       // /usr/share/syncwatch/omarchy-plugin
    QString userPlugins;   // ~/.config/omarchy/plugins
    QString omarchy;       // the omarchy command, empty when Omarchy is not there
    QString omarchyShell;  // omarchy-shell, to make the shell look for the new link
    QString declined;      // a file that says the user said no; empty never remembers

    static PluginPaths defaults();
};

inline constexpr char kPluginId[] = "syncwatch";
// The widget's id before it moved into SyncWatch.
inline constexpr char kOldPluginId[] = "cateim.drive-sync";

class PluginSetup : public QObject {
    Q_OBJECT
    // "checking" | "noOmarchy" | "notShipped" | "blocked" | "offer"
    // | "replaceOffer" | "enabled" | "declined" | "failed"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)

public:
    explicit PluginSetup(const PluginPaths &paths = PluginPaths::defaults(), QObject *parent = nullptr);
    ~PluginSetup() override;

    QString state() const { return m_state; }
    QString detail() const { return m_detail; }

    // Links the widget when the link is missing, then asks Omarchy whether
    // it is on the bar.
    Q_INVOKABLE void check();
    // Puts the widget on the bar, replacing the old one when it is there.
    // Only does something after check() offered it.
    Q_INVOKABLE void accept();
    // Remembers "no": later starts neither offer nor link the widget, so an
    // `omarchy plugin remove syncwatch` stays removed.
    Q_INVOKABLE void decline();

signals:
    void changed();

private:
    void setState(const QString &state, const QString &detail = QString());
    void readList();
    void run(const QString &program, const QStringList &arguments,
             std::function<void(bool ok, const QByteArray &output, const QByteArray &error)> done);

    PluginPaths m_paths;
    QString m_state = "checking";
    QString m_detail;
};
