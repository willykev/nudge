#include "store.h"

#include "recurrence.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QStandardPaths>
#include <QVariant>
#include <algorithm>

namespace {
QString isoDate(const QDate &date) { return date.isValid() ? date.toString(Qt::ISODate) : QString(); }
QDate fromIso(const QString &text) { return text.isEmpty() ? QDate() : QDate::fromString(text, Qt::ISODate); }
QVariant idOrNull(qint64 id) { return id ? QVariant(id) : QVariant(QMetaType::fromType<qint64>()); }
} // namespace

bool Reminder::sameContent(const Reminder &other) const {
    Reminder a = *this, b = other;
    a.alertedDue = a.alertedEarly = a.snoozeUntil = 0;
    b.alertedDue = b.alertedEarly = b.snoozeUntil = 0;
    return a == b;
}

qint64 Reminder::dueAt() const {
    if (!due.isValid())
        return 0;
    const int minutes = dueMinutes >= 0 ? dueMinutes : allDayAlertMinutes;
    return QDateTime(due, QTime(minutes / 60, minutes % 60)).toSecsSinceEpoch();
}

QString Store::defaultPath() {
    const QString override = qEnvironmentVariable("NUDGE_DB");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/nudge/nudge.db";
}

Store::Store(const QString &path, QObject *parent) : QObject(parent) {
    static int connections = 0;
    m_connection = QString("nudge-%1").arg(++connections);
    m_path = path.isEmpty() ? defaultPath() : path;

    m_reload.setSingleShot(true);
    m_reload.setInterval(60);
    connect(&m_reload, &QTimer::timeout, this, [this] {
        arm();
        reloadIfChanged();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reload.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reload.start(); });

    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        fail(QString("Could not create %1").arg(QFileInfo(m_path).absolutePath()));
        return;
    }
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_db.setDatabaseName(m_path);
    // The window, the command line, and the background alerts may all write.
    m_db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=4000");
    if (!m_db.open()) {
        fail(QString("Could not open %1: %2").arg(m_path, m_db.lastError().text()));
        return;
    }

    const bool ready =
        // Every change is on the disk before it shows as saved: a laptop can
        // lose power, and a reminder that vanished is worse than a millisecond.
        run("PRAGMA journal_mode=WAL") && run("PRAGMA synchronous=FULL") &&
        run("CREATE TABLE IF NOT EXISTS lists ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, color TEXT NOT NULL,"
            "position REAL NOT NULL, sort TEXT NOT NULL DEFAULT 'manual')") &&
        run("CREATE TABLE IF NOT EXISTS sections ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, list_id INTEGER NOT NULL, name TEXT NOT NULL,"
            "position REAL NOT NULL)") &&
        run("CREATE TABLE IF NOT EXISTS reminders ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, list_id INTEGER NOT NULL,"
            "section_id INTEGER NOT NULL DEFAULT 0, parent_id INTEGER NOT NULL DEFAULT 0,"
            "title TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '', url TEXT NOT NULL DEFAULT '',"
            "due TEXT NOT NULL DEFAULT '', due_minutes INTEGER NOT NULL DEFAULT -1,"
            "repeat TEXT NOT NULL DEFAULT '', repeat_end TEXT NOT NULL DEFAULT '',"
            "early INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 0,"
            "flagged INTEGER NOT NULL DEFAULT 0, tags TEXT NOT NULL DEFAULT '',"
            "completed INTEGER NOT NULL DEFAULT 0, completed_at INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL DEFAULT 0, modified_at INTEGER NOT NULL DEFAULT 0,"
            "position REAL NOT NULL DEFAULT 0, alerted_due INTEGER NOT NULL DEFAULT 0,"
            "alerted_early INTEGER NOT NULL DEFAULT 0, snooze_until INTEGER NOT NULL DEFAULT 0)") &&
        run("CREATE INDEX IF NOT EXISTS reminders_by_list ON reminders (list_id)") &&
        run("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)") &&
        run("PRAGMA user_version=1");
    if (!ready)
        return;

    prepare();
    load();
    m_open = true;
}

Store::~Store() {
    // Queries must let go of the connection before it can be removed.
    m_putReminder = QSqlQuery();
    m_putList = QSqlQuery();
    m_putSection = QSqlQuery();
    m_setAlerted = QSqlQuery();
    if (m_db.isValid()) {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool Store::run(const QString &sql) {
    QSqlQuery query(m_db);
    if (query.exec(sql))
        return true;
    fail(query.lastError().text());
    return false;
}

void Store::fail(const QString &message) {
    m_error = message;
    emit failed(message);
}

void Store::prepare() {
    m_putReminder = QSqlQuery(m_db);
    m_putReminder.prepare(
        "INSERT OR REPLACE INTO reminders (id, list_id, section_id, parent_id, title, notes, url, due,"
        "due_minutes, repeat, repeat_end, early, priority, flagged, tags, completed, completed_at,"
        "created_at, modified_at, position, alerted_due, alerted_early, snooze_until)"
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    m_putList = QSqlQuery(m_db);
    m_putList.prepare("INSERT OR REPLACE INTO lists (id, name, color, position, sort) VALUES (?, ?, ?, ?, ?)");
    m_putSection = QSqlQuery(m_db);
    m_putSection.prepare("INSERT OR REPLACE INTO sections (id, list_id, name, position) VALUES (?, ?, ?, ?)");
    m_setAlerted = QSqlQuery(m_db);
    m_setAlerted.prepare("UPDATE reminders SET alerted_due = ?, alerted_early = ?, snooze_until = ? WHERE id = ?");
}

void Store::load() {
    m_lists.clear();
    m_sections.clear();
    m_reminders.clear();

    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    if (query.exec("SELECT id, name, color, position, sort FROM lists")) {
        while (query.next()) {
            TaskList list;
            list.id = query.value(0).toLongLong();
            list.name = query.value(1).toString();
            list.color = query.value(2).toString();
            list.position = query.value(3).toDouble();
            list.sort = query.value(4).toString();
            m_lists.append(list);
        }
    }
    if (query.exec("SELECT id, list_id, name, position FROM sections")) {
        while (query.next()) {
            Section section;
            section.id = query.value(0).toLongLong();
            section.listId = query.value(1).toLongLong();
            section.name = query.value(2).toString();
            section.position = query.value(3).toDouble();
            m_sections.append(section);
        }
    }
    if (query.exec("SELECT id, list_id, section_id, parent_id, title, notes, url, due, due_minutes, repeat,"
                   "repeat_end, early, priority, flagged, tags, completed, completed_at, created_at,"
                   "modified_at, position, alerted_due, alerted_early, snooze_until FROM reminders")) {
        while (query.next()) {
            Reminder r;
            int i = 0;
            r.id = query.value(i++).toLongLong();
            r.listId = query.value(i++).toLongLong();
            r.sectionId = query.value(i++).toLongLong();
            r.parentId = query.value(i++).toLongLong();
            r.title = query.value(i++).toString();
            r.notes = query.value(i++).toString();
            r.url = query.value(i++).toString();
            r.due = fromIso(query.value(i++).toString());
            r.dueMinutes = query.value(i++).toInt();
            r.repeat = query.value(i++).toString();
            r.repeatEnd = fromIso(query.value(i++).toString());
            r.early = query.value(i++).toInt();
            r.priority = query.value(i++).toInt();
            r.flagged = query.value(i++).toBool();
            r.tags = query.value(i++).toString().split(' ', Qt::SkipEmptyParts);
            r.completed = query.value(i++).toBool();
            r.completedAt = query.value(i++).toLongLong();
            r.createdAt = query.value(i++).toLongLong();
            r.modifiedAt = query.value(i++).toLongLong();
            r.position = query.value(i++).toDouble();
            r.alertedDue = query.value(i++).toLongLong();
            r.alertedEarly = query.value(i++).toLongLong();
            r.snoozeUntil = query.value(i++).toLongLong();
            m_reminders.insert(r.id, r);
        }
    }
    query.finish();
    sortLists();
    m_dataVersion = dataVersion();
}

void Store::sortLists() {
    std::sort(m_lists.begin(), m_lists.end(), [](const TaskList &a, const TaskList &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
    std::sort(m_sections.begin(), m_sections.end(), [](const Section &a, const Section &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
}

// SQLite bumps this whenever another connection commits, never for our own.
qint64 Store::dataVersion() {
    QSqlQuery query(m_db);
    return query.exec("PRAGMA data_version") && query.next() ? query.value(0).toLongLong() : 0;
}

const TaskList *Store::list(qint64 id) const {
    for (const auto &list : m_lists)
        if (list.id == id)
            return &list;
    return nullptr;
}

const Section *Store::section(qint64 id) const {
    for (const auto &section : m_sections)
        if (section.id == id)
            return &section;
    return nullptr;
}

const Reminder *Store::reminder(qint64 id) const {
    const auto found = m_reminders.constFind(id);
    return found == m_reminders.constEnd() ? nullptr : &*found;
}

QVector<qint64> Store::childrenOf(qint64 id) const {
    QVector<const Reminder *> children;
    for (const auto &r : m_reminders)
        if (r.parentId == id)
            children.append(&r);
    std::sort(children.begin(), children.end(), [](const Reminder *a, const Reminder *b) {
        return a->position != b->position ? a->position < b->position : a->id < b->id;
    });
    QVector<qint64> ids;
    for (const auto *child : children)
        ids.append(child->id);
    return ids;
}

double Store::endPosition(qint64 listId) const {
    double last = 0;
    for (const auto &r : m_reminders)
        if (r.listId == listId)
            last = qMax(last, r.position);
    return last + 1;
}

// There is always somewhere for a new reminder to go. The first list is made
// quietly: it is not something to undo.
qint64 Store::defaultListId() {
    if (!m_lists.isEmpty())
        return m_lists.first().id;
    TaskList list;
    list.name = QCoreApplication::translate("Store", "Reminders");
    list.color = "blue";
    list.position = 1;
    m_recording = false;
    const qint64 id = put(list);
    m_recording = true;
    return id;
}

void Store::begin(const QString &mergeKey) {
    if (!m_open)
        return;
    if (m_depth++ == 0) {
        m_pending = UndoEntry();
        m_pending.mergeKey = mergeKey;
        m_db.transaction();
    }
}

void Store::commit() {
    if (!m_open || m_depth == 0 || --m_depth > 0)
        return;
    if (!m_db.commit()) {
        // Nothing reached the disk, so go back to what is there.
        fail(m_db.lastError().text());
        m_db.rollback();
        load();
        m_pending = UndoEntry();
        emit changed();
        return;
    }
    if (m_pending.isEmpty())
        return;
    if (m_recording) {
        const bool merge = !m_pending.mergeKey.isEmpty() && !m_undo.isEmpty() &&
                           m_undo.last().mergeKey == m_pending.mergeKey;
        if (merge) {
            m_undo.last().lists += m_pending.lists;
            m_undo.last().sections += m_pending.sections;
            m_undo.last().reminders += m_pending.reminders;
        } else {
            m_undo.append(m_pending);
            if (m_undo.size() > 200)
                m_undo.removeFirst();
        }
        m_redo.clear();
        emit undoChanged();
    }
    m_pending = UndoEntry();
    emit changed();
}

bool Store::write(Reminder &r) {
    QSqlQuery &query = m_putReminder;
    int i = 0;
    query.bindValue(i++, idOrNull(r.id));
    query.bindValue(i++, r.listId);
    query.bindValue(i++, r.sectionId);
    query.bindValue(i++, r.parentId);
    query.bindValue(i++, r.title);
    query.bindValue(i++, r.notes);
    query.bindValue(i++, r.url);
    query.bindValue(i++, isoDate(r.due));
    query.bindValue(i++, r.dueMinutes);
    query.bindValue(i++, r.repeat);
    query.bindValue(i++, isoDate(r.repeatEnd));
    query.bindValue(i++, r.early);
    query.bindValue(i++, r.priority);
    query.bindValue(i++, r.flagged ? 1 : 0);
    query.bindValue(i++, r.tags.join(' '));
    query.bindValue(i++, r.completed ? 1 : 0);
    query.bindValue(i++, r.completedAt);
    query.bindValue(i++, r.createdAt);
    query.bindValue(i++, r.modifiedAt);
    query.bindValue(i++, r.position);
    query.bindValue(i++, r.alertedDue);
    query.bindValue(i++, r.alertedEarly);
    query.bindValue(i++, r.snoozeUntil);
    if (!query.exec()) {
        fail(query.lastError().text());
        return false;
    }
    if (!r.id)
        r.id = query.lastInsertId().toLongLong();
    return true;
}

bool Store::write(TaskList &list) {
    QSqlQuery &query = m_putList;
    query.bindValue(0, idOrNull(list.id));
    query.bindValue(1, list.name);
    query.bindValue(2, list.color);
    query.bindValue(3, list.position);
    query.bindValue(4, list.sort);
    if (!query.exec()) {
        fail(query.lastError().text());
        return false;
    }
    if (!list.id)
        list.id = query.lastInsertId().toLongLong();
    return true;
}

bool Store::write(Section &section) {
    QSqlQuery &query = m_putSection;
    query.bindValue(0, idOrNull(section.id));
    query.bindValue(1, section.listId);
    query.bindValue(2, section.name);
    query.bindValue(3, section.position);
    if (!query.exec()) {
        fail(query.lastError().text());
        return false;
    }
    if (!section.id)
        section.id = query.lastInsertId().toLongLong();
    return true;
}

void Store::erase(const char *table, qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QString("DELETE FROM %1 WHERE id = ?").arg(QLatin1String(table)));
    query.bindValue(0, id);
    if (!query.exec())
        fail(query.lastError().text());
}

qint64 Store::put(Reminder r) {
    if (!m_open)
        return 0;
    std::optional<Reminder> before;
    if (const Reminder *existing = r.id ? reminder(r.id) : nullptr)
        before = *existing;
    if (before && *before == r)
        return r.id;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!r.createdAt)
        r.createdAt = now;
    r.modifiedAt = now;
    // A new date or time is a new moment to alert at.
    if (before && (before->due != r.due || before->dueMinutes != r.dueMinutes))
        r.alertedDue = r.alertedEarly = r.snoozeUntil = 0;
    else if (before && before->early != r.early)
        r.alertedEarly = 0;

    begin();
    if (write(r)) {
        m_reminders.insert(r.id, r);
        m_pending.reminders.append({r.id, before, r});
    }
    commit();
    return r.id;
}

qint64 Store::put(TaskList list) {
    if (!m_open)
        return 0;
    std::optional<TaskList> before;
    if (const TaskList *existing = list.id ? this->list(list.id) : nullptr)
        before = *existing;
    if (before && *before == list)
        return list.id;
    begin();
    if (write(list)) {
        apply(list.id, std::optional<TaskList>(list));
        m_pending.lists.append({list.id, before, list});
    }
    commit();
    return list.id;
}

qint64 Store::put(Section section) {
    if (!m_open)
        return 0;
    std::optional<Section> before;
    if (const Section *existing = section.id ? this->section(section.id) : nullptr)
        before = *existing;
    if (before && *before == section)
        return section.id;
    begin();
    if (write(section)) {
        apply(section.id, std::optional<Section>(section));
        m_pending.sections.append({section.id, before, section});
    }
    commit();
    return section.id;
}

void Store::eraseReminder(qint64 id) {
    const Reminder *existing = reminder(id);
    if (!existing)
        return;
    m_pending.reminders.append({id, *existing, std::nullopt});
    erase("reminders", id);
    m_reminders.remove(id);
}

void Store::removeReminder(qint64 id) {
    if (!m_open || !reminder(id))
        return;
    begin();
    for (qint64 child : childrenOf(id))
        eraseReminder(child);
    eraseReminder(id);
    commit();
}

void Store::removeReminders(const QVector<qint64> &ids) {
    if (!m_open)
        return;
    begin();
    for (qint64 id : ids)
        eraseReminder(id);
    commit();
}

void Store::removeList(qint64 id) {
    const TaskList *existing = list(id);
    if (!m_open || !existing)
        return;
    const TaskList removed = *existing; // the vector it points into is about to change
    begin();
    QVector<qint64> doomed;
    for (const auto &r : m_reminders)
        if (r.listId == id)
            doomed.append(r.id);
    for (qint64 reminderId : doomed)
        eraseReminder(reminderId);
    const auto sections = m_sections;
    for (const auto &section : sections) {
        if (section.listId != id)
            continue;
        m_pending.sections.append({section.id, section, std::nullopt});
        erase("sections", section.id);
        apply(section.id, std::optional<Section>());
    }
    m_pending.lists.append({id, removed, std::nullopt});
    erase("lists", id);
    apply(id, std::optional<TaskList>());
    commit();
}

void Store::removeSection(qint64 id) {
    const Section *existing = section(id);
    if (!m_open || !existing)
        return;
    const Section removed = *existing;
    begin();
    QVector<Reminder> loose;
    for (const auto &r : m_reminders)
        if (r.sectionId == id)
            loose.append(r);
    for (auto r : loose) {
        r.sectionId = 0;
        put(r);
    }
    m_pending.sections.append({id, removed, std::nullopt});
    erase("sections", id);
    apply(id, std::optional<Section>());
    commit();
}

QDate Store::complete(qint64 id, const QDateTime &now) {
    const Reminder *existing = reminder(id);
    if (!m_open || !existing || existing->completed)
        return {};
    Reminder r = *existing;
    const qint64 seconds = now.toSecsSinceEpoch();
    const Repeat rule = Repeat::parse(r.repeat);
    const QDate next =
        rule.repeats() && r.due.isValid() ? rule.upcoming(r.due, now.date(), r.repeatEnd) : QDate();

    begin();
    const QVector<qint64> children = childrenOf(id);
    if (next.isValid()) {
        // What was done stays on record; the reminder itself moves on, with
        // its subtasks to do again.
        Reminder done = r;
        done.id = 0;
        done.repeat.clear();
        done.repeatEnd = QDate();
        done.completed = true;
        done.completedAt = seconds;
        put(done);
        r.due = next;
        put(r);
        for (qint64 childId : children) {
            Reminder child = *reminder(childId);
            child.completed = false;
            child.completedAt = 0;
            put(child);
        }
    } else {
        r.completed = true;
        r.completedAt = seconds;
        put(r);
        for (qint64 childId : children) {
            Reminder child = *reminder(childId);
            if (child.completed)
                continue;
            child.completed = true;
            child.completedAt = seconds;
            put(child);
        }
    }
    commit();
    return next;
}

void Store::reopen(qint64 id) {
    const Reminder *existing = reminder(id);
    if (!m_open || !existing || !existing->completed)
        return;
    Reminder r = *existing;
    r.completed = false;
    r.completedAt = 0;
    begin();
    put(r);
    // An open subtask means its parent is not finished either.
    if (const Reminder *parent = r.parentId ? reminder(r.parentId) : nullptr; parent && parent->completed) {
        Reminder p = *parent;
        p.completed = false;
        p.completedAt = 0;
        put(p);
    }
    commit();
}

void Store::apply(qint64 id, const std::optional<Reminder> &state) {
    if (!state) {
        erase("reminders", id);
        m_reminders.remove(id);
        return;
    }
    Reminder r = *state;
    // Undo restores what the reminder said, not which alerts have gone out
    // since: an alert sent in the meantime must not be sent again.
    if (const Reminder *current = reminder(id);
        current && current->due == r.due && current->dueMinutes == r.dueMinutes) {
        r.alertedDue = current->alertedDue;
        r.alertedEarly = current->alertedEarly;
        r.snoozeUntil = current->snoozeUntil;
    }
    if (write(r))
        m_reminders.insert(id, r);
}

void Store::apply(qint64 id, const std::optional<TaskList> &state) {
    for (int i = 0; i < m_lists.size(); ++i)
        if (m_lists[i].id == id) {
            m_lists.remove(i);
            break;
        }
    if (state)
        m_lists.append(*state);
    sortLists();
}

void Store::apply(qint64 id, const std::optional<Section> &state) {
    for (int i = 0; i < m_sections.size(); ++i)
        if (m_sections[i].id == id) {
            m_sections.remove(i);
            break;
        }
    if (state)
        m_sections.append(*state);
    sortLists();
}

void Store::replay(const UndoEntry &entry, bool forward) {
    auto each = [forward](const auto &deltas, auto &&restore) {
        if (forward)
            for (auto it = deltas.cbegin(); it != deltas.cend(); ++it)
                restore(it->id, it->after);
        else
            for (auto it = deltas.crbegin(); it != deltas.crend(); ++it)
                restore(it->id, it->before);
    };
    m_db.transaction();
    each(entry.lists, [this](qint64 id, const std::optional<TaskList> &state) {
        if (state) {
            TaskList list = *state;
            write(list);
        } else {
            erase("lists", id);
        }
        apply(id, state);
    });
    each(entry.sections, [this](qint64 id, const std::optional<Section> &state) {
        if (state) {
            Section section = *state;
            write(section);
        } else {
            erase("sections", id);
        }
        apply(id, state);
    });
    each(entry.reminders, [this](qint64 id, const std::optional<Reminder> &state) { apply(id, state); });
    if (!m_db.commit()) {
        fail(m_db.lastError().text());
        m_db.rollback();
        load();
    }
}

void Store::undo() {
    if (!m_open || m_depth > 0 || m_undo.isEmpty())
        return;
    const UndoEntry entry = m_undo.takeLast();
    replay(entry, false);
    m_redo.append(entry);
    emit undoChanged();
    emit changed();
}

void Store::redo() {
    if (!m_open || m_depth > 0 || m_redo.isEmpty())
        return;
    const UndoEntry entry = m_redo.takeLast();
    replay(entry, true);
    m_undo.append(entry);
    emit undoChanged();
    emit changed();
}

void Store::sealUndo() {
    if (!m_undo.isEmpty())
        m_undo.last().mergeKey.clear();
}

void Store::clearUndo() {
    m_undo.clear();
    m_redo.clear();
    emit undoChanged();
}

void Store::discardLast(const QString &mergeKey) {
    if (!m_open || m_depth > 0 || m_undo.isEmpty() || m_undo.last().mergeKey != mergeKey)
        return;
    replay(m_undo.takeLast(), false);
    emit undoChanged();
    emit changed();
}

void Store::setAlerted(qint64 id, qint64 due, qint64 early, qint64 snoozeUntil) {
    const auto found = m_reminders.find(id);
    if (!m_open || found == m_reminders.end())
        return;
    found->alertedDue = due;
    found->alertedEarly = early;
    found->snoozeUntil = snoozeUntil;
    m_setAlerted.bindValue(0, due);
    m_setAlerted.bindValue(1, early);
    m_setAlerted.bindValue(2, snoozeUntil);
    m_setAlerted.bindValue(3, id);
    if (!m_setAlerted.exec())
        fail(m_setAlerted.lastError().text());
}

QString Store::meta(const QString &key) const {
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM meta WHERE key = ?");
    query.bindValue(0, key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

void Store::setMeta(const QString &key, const QString &value) {
    if (!m_open)
        return;
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    query.bindValue(0, key);
    query.bindValue(1, value);
    if (!query.exec())
        fail(query.lastError().text());
}

void Store::watch() {
    m_watching = true;
    arm();
}

// SQLite replaces and removes its write-ahead file as it works, which drops
// it from the watch, so every change re-arms.
void Store::arm() {
    if (!m_watching)
        return;
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    for (const QString &path : {QFileInfo(m_path).absolutePath(), m_path, m_path + "-wal"})
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

bool Store::reloadIfChanged() {
    if (!m_open || m_depth > 0 || dataVersion() == m_dataVersion)
        return false;
    const auto lists = m_lists;
    const auto sections = m_sections;
    const auto reminders = m_reminders;
    load();

    bool same = lists == m_lists && sections == m_sections && reminders.size() == m_reminders.size();
    for (auto it = m_reminders.cbegin(); same && it != m_reminders.cend(); ++it) {
        const auto old = reminders.constFind(it.key());
        same = old != reminders.constEnd() && old->sameContent(*it);
    }
    // Another process only noting which alerts it sent is no reason to
    // disturb the window or forget what can be undone.
    if (same)
        return false;
    m_undo.clear();
    m_redo.clear();
    emit undoChanged();
    emit changed();
    return true;
}
