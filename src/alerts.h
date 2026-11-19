#pragma once

#include "store.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <optional>

// Where alerts go. The desktop's notifications in the app; a recorder in tests.
class Alerts {
public:
    virtual ~Alerts() = default;
    // Shows an alert and returns its id, or 0 when it could not be shown.
    // `actions` alternates keys and labels, as the notification spec has it.
    virtual quint32 show(const QString &title, const QString &body, const QStringList &actions) = 0;
    // Shows an alert again in its own place, the same but with no buttons:
    // for when the process that would answer them is going away.
    virtual void disarm(quint32 id, const QString &title, const QString &body);
    // Whether the body is read as markup, where a literal & or < has to be
    // escaped. The title never is.
    virtual bool bodyMarkup() { return false; }
    // Whether this process is the one to alert: only one of the window and
    // the background service should, or every reminder would arrive twice.
    virtual bool mayAlert() { return true; }
    virtual QString problem() const { return {}; }
};

// Brings an open Nudge window to the front, and says whether it did.
bool focusNudgeWindow(qint64 pid = 0);
void registerNudgeWindow(QObject *window);
bool openExistingNudgeWindow(const QString &database, qint64 id);

// Desktop notifications over D-Bus (org.freedesktop.Notifications).
class DesktopAlerts : public QObject, public Alerts {
    Q_OBJECT

public:
    explicit DesktopAlerts(bool background, QObject *parent = nullptr);

    quint32 show(const QString &title, const QString &body, const QStringList &actions) override;
    void disarm(quint32 id, const QString &title, const QString &body) override;
    bool bodyMarkup() override;
    bool mayAlert() override;
    QString problem() const override { return m_problem; }

signals:
    void acted(quint32 id, const QString &action);
    void closed(quint32 id); // dismissed, expired, or gone with the server

private slots:
    void onActionInvoked(uint id, const QString &action);
    void onNotificationClosed(uint id, uint reason);
    void onServerChanged(const QString &service, const QString &oldOwner, const QString &newOwner);

private:
    quint32 notify(const QString &title, const QString &body, const QStringList &actions, quint32 replaces);

    QString m_problem;
    QSet<quint32> m_open; // what this process has on screen
    std::optional<bool> m_markup;
};

// Watches the clock and announces reminders as they come due.
class Notifier : public QObject {
    Q_OBJECT

public:
    Notifier(Store *store, Alerts *alerts, QObject *parent = nullptr);

    void start();
    void check(const QDateTime &now);
    void act(quint32 notification, const QString &action, const QDateTime &now);
    void forget(quint32 notification); // it left the screen
    void disarm();                     // the process is quitting

signals:
    void opened(qint64 reminderId); // 0 when the alert was about several

private slots:
    void onPrepareForSleep(bool sleeping);

private:
    // An alert on screen: the reminder it announced (0 for several) and when
    // that was due then, the moment its buttons answer for.
    struct Shown {
        qint64 id = 0;
        qint64 dueAt = 0;
        QString title, body; // as shown, to show again without buttons
    };

    Store *m_store;
    Alerts *m_alerts;
    QTimer m_timer;
    QHash<quint32, Shown> m_shown;
    bool m_checking = false;
};
