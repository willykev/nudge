#include "alerts.h"

#include "dates.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QVariantMap>

namespace {
const QString notifications = QStringLiteral("org.freedesktop.Notifications");
const QString notificationsPath = QStringLiteral("/org/freedesktop/Notifications");
// Whoever owns this name on the session bus is the one that alerts.
const QString alerterName = QStringLiteral("io.nudge.Alerts");
} // namespace

// A Wayland window cannot raise itself, so the compositor is asked to. Without
// Hyprland there is no window to find, and the caller opens a new one.
bool focusNudgeWindow() {
    QProcess clients;
    clients.start("hyprctl", {"clients", "-j"});
    if (!clients.waitForFinished(1500))
        return false;
    for (const QJsonValue &value : QJsonDocument::fromJson(clients.readAllStandardOutput()).array()) {
        const QJsonObject client = value.toObject();
        if (client.value("class").toString() != "nudge")
            continue;
        const QString window = "address:" + client.value("address").toString();
        // Hyprland's Lua dispatchers, then the syntax of the versions before them.
        const QList<QStringList> attempts{{"dispatch", QString("hl.dsp.focus({ window = \"%1\" })").arg(window)},
                                          {"dispatch", "focuswindow", window}};
        bool asked = false;
        for (const QStringList &arguments : attempts) {
            QProcess focus;
            focus.start("hyprctl", arguments);
            asked = focus.waitForFinished(1500) && focus.readAllStandardOutput().trimmed() == "ok";
            if (asked)
                break;
        }
        // Hyprland answers "ok" even when the window stayed where it was (on a
        // hidden special workspace, say), so look at what has the focus now.
        QProcess active;
        active.start("hyprctl", {"-j", "activewindow"});
        if (asked && active.waitForFinished(1500) &&
            QJsonDocument::fromJson(active.readAllStandardOutput()).object().value("address") == client.value("address"))
            return true;
    }
    // None found, or none that would come forward: the caller opens a window
    // rather than leave a click with nothing to show for it.
    return false;
}

DesktopAlerts::DesktopAlerts(bool background, QObject *parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        m_problem = tr("No connection to the desktop's message bus, so reminders cannot show notifications.");
        return;
    }
    // The background service takes the name whenever it runs. A window
    // stands in line for it, and alerts only while no service is running.
    using Bus = QDBusConnectionInterface;
    bus.interface()->registerService(alerterName, background ? Bus::ReplaceExistingService : Bus::QueueService,
                                     background ? Bus::DontAllowReplacement : Bus::AllowReplacement);
    bus.connect(notifications, notificationsPath, notifications, "ActionInvoked", this,
                SLOT(onActionInvoked(uint,QString)));
}

bool DesktopAlerts::mayAlert() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    const QDBusReply<QString> owner = bus.interface()->serviceOwner(alerterName);
    return owner.isValid() && owner.value() == bus.baseService();
}

quint32 DesktopAlerts::show(const QString &title, const QString &body, const QStringList &actions) {
    QDBusMessage call = QDBusMessage::createMethodCall(notifications, notificationsPath, notifications, "Notify");
    const QVariantMap hints{{"desktop-entry", QStringLiteral("nudge")}, {"urgency", QVariant::fromValue<uchar>(1)}};
    // A reminder stays until it is dealt with: one that fades after five
    // seconds reminds nobody who stepped away.
    call.setArguments({QStringLiteral("Nudge"), uint(0), QStringLiteral("nudge"), title, body, actions, hints, int(0)});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        m_problem = tr("The desktop did not accept a notification: %1").arg(reply.errorMessage());
        return 0;
    }
    m_problem.clear();
    return reply.arguments().first().toUInt();
}

void DesktopAlerts::onActionInvoked(uint id, const QString &action) {
    emit acted(id, action);
}

Notifier::Notifier(Store *store, Alerts *alerts, QObject *parent) : QObject(parent), m_store(store), m_alerts(alerts) {
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { check(QDateTime::currentDateTime()); });
    connect(m_store, &Store::changed, this, [this] { check(QDateTime::currentDateTime()); });
    // Timers stand still while the machine sleeps. logind says when it wakes,
    // so what came due with the lid closed is announced as it opens.
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
                                         "org.freedesktop.login1.Manager", "PrepareForSleep", this,
                                         SLOT(onPrepareForSleep(bool)));
}

void Notifier::onPrepareForSleep(bool sleeping) {
    if (!sleeping)
        check(QDateTime::currentDateTime());
}

void Notifier::start() {
    check(QDateTime::currentDateTime());
}

void Notifier::check(const QDateTime &now) {
    const qint64 seconds = now.toSecsSinceEpoch();
    struct Pending {
        qint64 id;
        bool early, due, snoozed;
    };
    QVector<Pending> pending;
    qint64 nextAt = 0;
    auto later = [&](qint64 at) {
        if (at > seconds && (!nextAt || at < nextAt))
            nextAt = at;
    };

    for (const Reminder &r : m_store->reminders()) {
        // One still being typed has no title yet, and nothing to announce.
        if (r.completed || !r.due.isValid() || r.title.isEmpty())
            continue;
        const qint64 dueAt = r.dueAt();
        const qint64 earlyAt = r.early > 0 ? dueAt - 60LL * r.early : 0;
        later(dueAt);
        later(earlyAt);
        later(r.snoozeUntil);
        // A moment that had already passed when the reminder was last given
        // its date is not news: setting something for this morning in the
        // afternoon should not alert at once.
        const qint64 since = r.modifiedAt - 60;
        Pending p{r.id, false, false, false};
        p.early = earlyAt && earlyAt <= seconds && dueAt > seconds && r.alertedEarly != earlyAt && earlyAt >= since;
        p.due = dueAt <= seconds && r.alertedDue != dueAt && dueAt >= since;
        p.snoozed = r.snoozeUntil && r.snoozeUntil <= seconds;
        if (p.early || p.due || p.snoozed)
            pending.append(p);
    }

    if (!pending.isEmpty() && m_alerts->mayAlert()) {
        const QDate today = now.date();
        auto mark = [&](const Pending &p) {
            const Reminder *r = m_store->reminder(p.id);
            const qint64 dueAt = r->dueAt();
            m_store->setAlerted(p.id, p.due ? dueAt : r->alertedDue,
                                p.early ? dueAt - 60LL * r->early : r->alertedEarly, p.snoozed ? 0 : r->snoozeUntil);
        };
        if (pending.size() > 3) {
            // After a night off there may be a pile; say so once.
            QStringList titles;
            for (int i = 0; i < pending.size() && i < 5; ++i)
                titles << m_store->reminder(pending[i].id)->title;
            if (pending.size() > 5)
                titles << QStringLiteral("…");
            const quint32 shown = m_alerts->show(tr("%n reminders are due", "", pending.size()), titles.join('\n'),
                                                 {"default", tr("Open")});
            if (shown) {
                m_shown.insert(shown, 0);
                for (const Pending &p : pending)
                    mark(p);
            }
        } else {
            for (const Pending &p : pending) {
                const Reminder *r = m_store->reminder(p.id);
                const TaskList *list = m_store->list(r->listId);
                QStringList body{Dates::formatDue(r->due, r->dueMinutes, today)};
                if (list)
                    body << list->name;
                QString text = body.join(QStringLiteral(" · "));
                if (p.early && !p.due)
                    text = tr("Coming up: %1").arg(text);
                if (!r->notes.isEmpty())
                    text += '\n' + r->notes.section('\n', 0, 0);
                const quint32 shown =
                    m_alerts->show(r->title.isEmpty() ? tr("Reminder") : r->title, text,
                                   {"default", tr("Open"), "done", tr("Complete"), "hour", tr("In an Hour"),
                                    "tomorrow", tr("Tomorrow")});
                if (shown) {
                    m_shown.insert(shown, p.id);
                    mark(p);
                }
            }
        }
    }

    // Wake for the next alert, and at least every minute in case the clock
    // jumped or the machine slept.
    const qint64 wait = nextAt ? qBound(qint64(1), nextAt - seconds, qint64(60)) : 60;
    m_timer.start(int(wait * 1000) + 200);
}

void Notifier::act(quint32 notification, const QString &action, const QDateTime &now) {
    if (!m_shown.contains(notification))
        return;
    const qint64 id = m_shown.value(notification);
    if (action == "default") {
        emit opened(id);
        return;
    }
    m_shown.remove(notification);
    const Reminder *r = m_store->reminder(id);
    if (!r)
        return;
    if (action == "done") {
        m_store->complete(id, now);
    } else if (action == "hour") {
        m_store->setAlerted(id, r->alertedDue, r->alertedEarly, now.toSecsSinceEpoch() + 3600);
    } else if (action == "tomorrow") {
        const QDateTime morning(now.date().addDays(1), QTime(Reminder::allDayAlertMinutes / 60, 0));
        m_store->setAlerted(id, r->alertedDue, r->alertedEarly, morning.toSecsSinceEpoch());
    }
    check(now);
}
