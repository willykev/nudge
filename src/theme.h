#pragma once

#include <QFileSystemWatcher>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Hyprland's word that it has reloaded its config, as a theme switch, a
// toggle and the person's own edit all make it do. It comes once the new
// values are in effect, so it is the moment to ask hyprctl again.
class HyprlandReloads : public QObject {
    Q_OBJECT

public:
    explicit HyprlandReloads(QObject *parent = nullptr);

    // Listens on an event socket; an empty path listens to nothing.
    void listen(const QString &socket);
    // The running Hyprland's event socket, or nothing outside Hyprland.
    static QString socket();

signals:
    void reloaded();

private:
    QLocalSocket m_socket;
    QString m_path;
};

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &hyprlandEvents = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }

    static QString readAccent(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingTimeout;
    bool m_roundingAgain = false;
    HyprlandReloads m_reloads;
};
