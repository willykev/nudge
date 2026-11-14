#pragma once

#include "store.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

// Where alerts go. The desktop's notifications in the app; a recorder in tests.
class Alerts {
public:
    virtual ~Alerts() = default;
    // Shows an alert and returns its id, or 0 when it could not be shown.
    // `actions` alternates keys and labels, as the notification spec has it.
    virtual quint32 show(const QString &title, const QString &body, const QStringList &actions) = 0;
    // Whether this process is the one to alert: only one of the window and
    // the background service should, or every reminder would arrive twice.
    virtual bool mayAlert() { return true; }
    virtual QString problem() const { return {}; }
};

// Brings an open Nudge window to the front, and says whether it did.
bool focusNudgeWindow();

// Desktop notifications over D-Bus (org.freedesktop.Notifications).
class DesktopAlerts : public QObject, public Alerts {
    Q_OBJECT

public:
    explicit DesktopAlerts(bool background, QObject *parent = nullptr);

    quint32 show(const QString &title, const QString &body, const QStringList &actions) override;
    bool mayAlert() override;
    QString problem() const override { return m_problem; }

signals:
    void acted(quint32 id, const QString &action);

private slots:
    void onActionInvoked(uint id, const QString &action);

private:
    QString m_problem;
};

// Watches the clock and announces reminders as they come due.
class Notifier : public QObject {
    Q_OBJECT

public:
    Notifier(Store *store, Alerts *alerts, QObject *parent = nullptr);

    void start();
    void check(const QDateTime &now);
    void act(quint32 notification, const QString &action, const QDateTime &now);

signals:
    void opened(qint64 reminderId); // 0 when the alert was about several

private slots:
    void onPrepareForSleep(bool sleeping);

private:
    Store *m_store;
    Alerts *m_alerts;
    QTimer m_timer;
    QHash<quint32, qint64> m_shown;
};
