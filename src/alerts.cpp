#include "alerts.h"

#include "dates.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
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

// Alerts that cannot be changed once shown are left as they are.
void Alerts::disarm(quint32, const QString &, const QString &) {}

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
    bus.connect(notifications, notificationsPath, notifications, "NotificationClosed", this,
                SLOT(onNotificationClosed(uint,uint)));
    auto *server = new QDBusServiceWatcher(notifications, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(server, &QDBusServiceWatcher::serviceOwnerChanged, this, &DesktopAlerts::onServerChanged);
}

bool DesktopAlerts::mayAlert() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    const QDBusReply<QString> owner = bus.interface()->serviceOwner(alerterName);
    return owner.isValid() && owner.value() == bus.baseService();
}

quint32 DesktopAlerts::show(const QString &title, const QString &body, const QStringList &actions) {
    const quint32 id = notify(title, body, actions, 0);
    if (id)
        m_open.insert(id);
    return id;
}

void DesktopAlerts::disarm(quint32 id, const QString &title, const QString &body) {
    // Only one still on screen: a server given an id it no longer has shows
    // the alert anew, and one the user dismissed would be back.
    if (m_open.remove(id))
        notify(title, body, {}, id);
}

quint32 DesktopAlerts::notify(const QString &title, const QString &body, const QStringList &actions,
                              quint32 replaces) {
    QDBusMessage call = QDBusMessage::createMethodCall(notifications, notificationsPath, notifications, "Notify");
    const QVariantMap hints{{"desktop-entry", QStringLiteral("nudge")}, {"urgency", QVariant::fromValue<uchar>(1)}};
    // A reminder stays until it is dealt with: one that fades after five
    // seconds reminds nobody who stepped away.
    call.setArguments({QStringLiteral("Nudge"), uint(replaces), QStringLiteral("nudge"), title, body, actions, hints,
                       int(0)});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        m_problem = tr("The desktop did not accept a notification: %1").arg(reply.errorMessage());
        return 0;
    }
    m_problem.clear();
    return reply.arguments().first().toUInt();
}

// Asked once of each notification server. One that cannot say is sent plain
// text, which every server shows as it is.
bool DesktopAlerts::bodyMarkup() {
    if (!m_markup) {
        const QDBusReply<QStringList> capabilities = QDBusConnection::sessionBus().call(
            QDBusMessage::createMethodCall(notifications, notificationsPath, notifications, "GetCapabilities"),
            QDBus::Block, 3000);
        m_markup = capabilities.isValid() && capabilities.value().contains("body-markup");
    }
    return *m_markup;
}

void DesktopAlerts::onActionInvoked(uint id, const QString &action) {
    emit acted(id, action);
}

void DesktopAlerts::onNotificationClosed(uint id, uint) {
    if (m_open.remove(id))
        emit closed(id);
}

// A notification server that quits takes every alert on screen with it, and
// the next one counts its ids from the start again, so ours would come to
// name other apps' notifications. It may read the body differently, too.
void DesktopAlerts::onServerChanged(const QString &, const QString &oldOwner, const QString &) {
    m_markup.reset();
    if (oldOwner.isEmpty())
        return;
    const QSet<quint32> gone = m_open;
    m_open.clear();
    for (quint32 id : gone)
        emit closed(id);
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
    // The buttons on an alert are answered here and nowhere else: they go
    // when the process quits, and are let go once the alert leaves the screen.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &Notifier::disarm);
    if (auto *desktop = dynamic_cast<DesktopAlerts *>(alerts))
        connect(desktop, &DesktopAlerts::closed, this, &Notifier::forget);
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
        // afternoon should not alert at once. Other edits, like its notes,
        // leave that alone.
        const qint64 since = r.scheduledSince() - 60;
        Pending p{r.id, false, false, false};
        p.early = earlyAt && earlyAt <= seconds && dueAt > seconds && r.alertedEarly != earlyAt && earlyAt >= since;
        p.due = dueAt <= seconds && r.alertedDue != dueAt && dueAt >= since;
        p.snoozed = r.snoozeUntil && r.snoozeUntil <= seconds;
        if (p.early || p.due || p.snoozed)
            pending.append(p);
    }

    if (!pending.isEmpty() && m_alerts->mayAlert()) {
        const QDate today = now.date();
        // Where the desktop reads the body as markup, a & or < in a title, a
        // list name or a note would be taken for markup of its own. Nothing in
        // the body is meant as markup, so there all of it is escaped.
        const bool markup = m_alerts->bodyMarkup();
        auto asBody = [markup](const QString &text) { return markup ? text.toHtmlEscaped() : text; };
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
            const QString title = tr("%n reminders are due", "", pending.size());
            const QString body = asBody(titles.join('\n'));
            const quint32 shown = m_alerts->show(title, body, {"default", tr("Open")});
            if (shown) {
                m_shown.insert(shown, {0, {}, -1, title, body});
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
                text = asBody(text);
                const QString title = r->title.isEmpty() ? tr("Reminder") : r->title;
                const quint32 shown =
                    m_alerts->show(title, text,
                                   {"default", tr("Open"), "done", tr("Complete"), "hour", tr("In an Hour"),
                                    "tomorrow", tr("Tomorrow")});
                if (shown) {
                    m_shown.insert(shown, {p.id, r->due, r->dueMinutes, title, text});
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
    const Shown shown = m_shown.value(notification);
    const qint64 id = shown.id;
    if (action == "default") {
        emit opened(id);
        return;
    }
    m_shown.remove(notification);
    // The buttons answer for the moment the alert announced. Once that was
    // dealt with some other way (the reminder completed, given another date
    // or deleted, or a repeat gone on to its next date), a late click must
    // not complete or snooze what comes after it. Another process may have
    // dealt with it a moment ago, so what is on disk decides.
    m_store->reloadIfChanged();
    const Reminder *r = m_store->reminder(id);
    if (!r || r->completed || r->due != shown.due || r->dueMinutes != shown.dueMinutes)
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

void Notifier::forget(quint32 notification) {
    m_shown.remove(notification);
}

// Nothing answers the buttons on this process's alerts once it has quit, and
// without the background service nothing else ever will. So as it quits,
// each alert it still has on screen is shown again without them: what came
// due stays in sight until it is dealt with, and no button is left dead.
void Notifier::disarm() {
    for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it)
        m_alerts->disarm(it.key(), it->title, it->body);
    m_shown.clear();
}
