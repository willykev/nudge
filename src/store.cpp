#include "store.h"

#include "recurrence.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QStandardPaths>
#include <QVariant>
#include <algorithm>

namespace {
QString isoDate(const QDate &date) { return date.isValid() ? date.toString(Qt::ISODate) : QString(); }
QDate fromIso(const QString &text) { return text.isEmpty() ? QDate() : QDate::fromString(text, Qt::ISODate); }
QVariant idOrNull(qint64 id) { return id ? QVariant(id) : QVariant(QMetaType::fromType<qint64>()); }

const char *const reminderColumns =
    "id, list_id, section_id, parent_id, title, notes, url, due, due_minutes, repeat,"
    "repeat_end, early, priority, flagged, tags, completed, completed_at, created_at,"
    "modified_at, position, alerted_due, alerted_early, snooze_until, scheduled_at";

Reminder reminderFrom(const QSqlQuery &query) {
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
    r.scheduledAt = query.value(i++).toLongLong();
    return r;
}

TaskList listFrom(const QSqlQuery &query) {
    TaskList list;
    list.id = query.value(0).toLongLong();
    list.name = query.value(1).toString();
    list.color = query.value(2).toString();
    list.position = query.value(3).toDouble();
    list.sort = query.value(4).toString();
    return list;
}

Section sectionFrom(const QSqlQuery &query) {
    Section section;
    section.id = query.value(0).toLongLong();
    section.listId = query.value(1).toLongLong();
    section.name = query.value(2).toString();
    section.position = query.value(3).toDouble();
    return section;
}

// What `after` changed of `before`, on top of `current`: each field this
// process changed takes its value, and every other field stays as another
// process left it. A field that goes with another moves with it.
#define TAKE(field) if (after.field != before.field) merged.field = after.field
#define TAKE2(a, b)                                                                                                    \
    if (after.a != before.a || after.b != before.b) {                                                                  \
        merged.a = after.a;                                                                                            \
        merged.b = after.b;                                                                                            \
    }
Reminder rebased(const Reminder &current, const Reminder &before, const Reminder &after) {
    Reminder merged = current;
    TAKE(listId);
    TAKE(sectionId);
    TAKE(parentId);
    TAKE(title);
    TAKE(notes);
    TAKE(url);
    TAKE2(due, dueMinutes);
    TAKE2(repeat, repeatEnd);
    TAKE(early);
    TAKE(priority);
    TAKE(flagged);
    TAKE(tags);
    TAKE2(completed, completedAt);
    TAKE(position);
    TAKE(alertedDue);
    TAKE(alertedEarly);
    TAKE(snoozeUntil);
    return merged;
}

TaskList rebased(const TaskList &current, const TaskList &before, const TaskList &after) {
    TaskList merged = current;
    TAKE(name);
    TAKE(color);
    TAKE(position);
    TAKE(sort);
    return merged;
}

Section rebased(const Section &current, const Section &before, const Section &after) {
    Section merged = current;
    TAKE(listId);
    TAKE(name);
    TAKE(position);
    return merged;
}
#undef TAKE
#undef TAKE2
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

    const QString folder = QFileInfo(m_path).absolutePath();
    const bool newFolder = !QFileInfo::exists(folder);
    if (!QDir().mkpath(folder)) {
        fail(QString("Could not create %1").arg(folder));
        return;
    }
    // What is written down here is the person's own: the folder Nudge makes,
    // or its own one, is theirs alone, and so is the file.
    if (newFolder || QFileInfo(defaultPath()).absolutePath() == folder)
        QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    keepPrivate();
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
            "alerted_early INTEGER NOT NULL DEFAULT 0, snooze_until INTEGER NOT NULL DEFAULT 0,"
            "scheduled_at INTEGER NOT NULL DEFAULT 0)") &&
        // Files from Nudge 0.1.3 and earlier: until a reminder is given a
        // moment anew, it was last given one when it was last changed.
        (hasColumn("reminders", "scheduled_at") ||
         (run("ALTER TABLE reminders ADD COLUMN scheduled_at INTEGER NOT NULL DEFAULT 0") &&
          run("UPDATE reminders SET scheduled_at = modified_at"))) &&
        run("CREATE INDEX IF NOT EXISTS reminders_by_list ON reminders (list_id)") &&
        run("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)") &&
        // Set only when it differs: any write tells every other process the
        // file changed, and a command that only reads should not.
        (pragma("user_version") == 1 || run("PRAGMA user_version=1"));
    if (!ready)
        return;

    prepare();
    if (!load())
        return;
    m_open = true;
    repair();
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

bool Store::hasColumn(const QString &table, const QString &column) {
    QSqlQuery query(m_db);
    if (!query.exec(QString("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (query.next())
        if (query.value(1).toString() == column)
            return true;
    return false;
}

// The file, and the two SQLite keeps beside it (which it makes with the
// file's own permissions), are made readable by their owner alone. A file
// that is not there yet is made empty first, so it never exists otherwise.
void Store::keepPrivate() {
    const auto own = QFile::ReadOwner | QFile::WriteOwner;
    QFile file(m_path);
    if (!QFileInfo::exists(m_path) && file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        file.setPermissions(own);
        file.close();
    }
    // Files an earlier Nudge made readable by everyone are made private too.
    for (const QString &path : {m_path, m_path + "-wal", m_path + "-shm"})
        if (QFileInfo(path).isFile())
            QFile::setPermissions(path, own);
}

void Store::fail(const QString &message) {
    m_error = message;
    emit failed(message);
}

void Store::prepare() {
    m_putReminder = QSqlQuery(m_db);
    m_putReminder.prepare(
        QString("INSERT OR REPLACE INTO reminders (%1) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
            .arg(QLatin1String(reminderColumns)));
    m_putList = QSqlQuery(m_db);
    m_putList.prepare("INSERT OR REPLACE INTO lists (id, name, color, position, sort) VALUES (?, ?, ?, ?, ?)");
    m_putSection = QSqlQuery(m_db);
    m_putSection.prepare("INSERT OR REPLACE INTO sections (id, list_id, name, position) VALUES (?, ?, ?, ?)");
    m_setAlerted = QSqlQuery(m_db);
    m_setAlerted.prepare("UPDATE reminders SET alerted_due = ?, alerted_early = ?, snooze_until = ? WHERE id = ?");
}

// Everything is read in one read transaction, so it is one moment of the
// file, and the version noted is that moment's: a commit landing meanwhile
// is still news next time. Memory stays as it was when the file cannot be read.
bool Store::load() {
    QVector<TaskList> lists;
    QVector<Section> sections;
    QHash<qint64, Reminder> reminders;
    const bool reading = m_db.transaction();
    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    bool ok = reading && query.exec("PRAGMA data_version") && query.next();
    const qint64 version = ok ? query.value(0).toLongLong() : 0;
    if (ok && query.exec("SELECT id, name, color, position, sort FROM lists")) {
        while (query.next())
            lists.append(listFrom(query));
    }
    ok = ok && !query.lastError().isValid();
    if (m_loadHook)
        m_loadHook();
    if (ok && query.exec("SELECT id, list_id, name, position FROM sections")) {
        while (query.next())
            sections.append(sectionFrom(query));
    }
    ok = ok && !query.lastError().isValid();
    if (ok && query.exec(QString("SELECT %1 FROM reminders").arg(QLatin1String(reminderColumns)))) {
        while (query.next()) {
            const Reminder r = reminderFrom(query);
            reminders.insert(r.id, r);
        }
    }
    ok = ok && !query.lastError().isValid();
    const QString problem = query.lastError().isValid() ? query.lastError().text() : m_db.lastError().text();
    query.finish();
    if (reading)
        m_db.commit(); // it only read
    if (!ok) {
        fail(problem);
        return false;
    }
    m_lists = std::move(lists);
    m_sections = std::move(sections);
    m_reminders = std::move(reminders);
    m_dataVersion = version;
    sortLists();
    return true;
}

// Nudge 0.1.0 could leave a completed reminder with open subtasks, which no
// view showed. Such parents are open again, quietly: there is nothing to undo.
void Store::repair() {
    QSet<qint64> parents;
    for (const Reminder &r : std::as_const(m_reminders))
        if (const Reminder *parent = r.completed ? nullptr : reminder(r.parentId); parent && parent->completed)
            parents.insert(parent->id);
    if (parents.isEmpty())
        return;
    m_recording = false;
    begin();
    for (const qint64 id : std::as_const(parents)) {
        Reminder parent = *reminder(id);
        parent.completed = false;
        parent.completedAt = 0;
        put(parent);
    }
    commit();
    m_recording = true;
}

void Store::sortLists() {
    std::sort(m_lists.begin(), m_lists.end(), [](const TaskList &a, const TaskList &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
    std::sort(m_sections.begin(), m_sections.end(), [](const Section &a, const Section &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
}

qint64 Store::pragma(const char *name) {
    QSqlQuery query(m_db);
    return query.exec(QString("PRAGMA %1").arg(QLatin1String(name))) && query.next() ? query.value(0).toLongLong()
                                                                                      : -1;
}

// SQLite bumps this whenever another connection commits, never for our own.
qint64 Store::dataVersion() {
    return pragma("data_version");
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
        // Outside a transaction each write would stand on its own, so none is
        // tried. The write lock is taken at once: a row is read again before
        // it is written (see rebase()), and nothing may change it in between.
        if (!run("BEGIN IMMEDIATE"))
            m_broken = true;
    }
}

bool Store::commit() {
    if (!m_open || m_depth == 0)
        return false;
    if (--m_depth > 0)
        return !m_broken;
    if (!finish())
        return false;
    if (m_pending.isEmpty())
        return true;
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
    return true;
}

// Ends the outermost transaction. All of it reaches the disk, or none of it
// stays: memory goes back to match, and nothing of it waits to be undone.
bool Store::finish() {
    if (!m_broken && m_db.commit())
        return true;
    if (!m_broken)
        fail(m_db.lastError().text());
    m_db.rollback();
    m_broken = false;
    revert(m_pending);
    m_pending = UndoEntry();
    // Another process may have written meanwhile.
    if (!reloadIfChanged())
        emit changed();
    return false;
}

// Takes back in memory what a transaction that never reached the disk did there.
void Store::revert(const UndoEntry &entry) {
    for (auto it = entry.reminders.crbegin(); it != entry.reminders.crend(); ++it) {
        if (it->before)
            m_reminders.insert(it->id, *it->before);
        else
            m_reminders.remove(it->id);
    }
    for (auto it = entry.sections.crbegin(); it != entry.sections.crend(); ++it)
        apply(it->id, it->before);
    for (auto it = entry.lists.crbegin(); it != entry.lists.crend(); ++it)
        apply(it->id, it->before);
}

bool Store::write(Reminder &r) {
    if (m_broken)
        return false;
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
    query.bindValue(i++, r.scheduledAt);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return false;
    }
    if (!r.id)
        r.id = query.lastInsertId().toLongLong();
    return true;
}

bool Store::write(TaskList &list) {
    if (m_broken)
        return false;
    QSqlQuery &query = m_putList;
    query.bindValue(0, idOrNull(list.id));
    query.bindValue(1, list.name);
    query.bindValue(2, list.color);
    query.bindValue(3, list.position);
    query.bindValue(4, list.sort);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return false;
    }
    if (!list.id)
        list.id = query.lastInsertId().toLongLong();
    return true;
}

bool Store::write(Section &section) {
    if (m_broken)
        return false;
    QSqlQuery &query = m_putSection;
    query.bindValue(0, idOrNull(section.id));
    query.bindValue(1, section.listId);
    query.bindValue(2, section.name);
    query.bindValue(3, section.position);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return false;
    }
    if (!section.id)
        section.id = query.lastInsertId().toLongLong();
    return true;
}

bool Store::erase(const char *table, qint64 id) {
    if (m_broken)
        return false;
    QSqlQuery query(m_db);
    query.prepare(QString("DELETE FROM %1 WHERE id = ?").arg(QLatin1String(table)));
    query.bindValue(0, id);
    if (query.exec())
        return true;
    fail(query.lastError().text());
    m_broken = true;
    return false;
}

std::optional<Reminder> Store::fetchReminder(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QString("SELECT %1 FROM reminders WHERE id = ?").arg(QLatin1String(reminderColumns)));
    query.bindValue(0, id);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return std::nullopt;
    }
    return query.next() ? std::optional<Reminder>(reminderFrom(query)) : std::nullopt;
}

std::optional<TaskList> Store::fetchList(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare("SELECT id, name, color, position, sort FROM lists WHERE id = ?");
    query.bindValue(0, id);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return std::nullopt;
    }
    return query.next() ? std::optional<TaskList>(listFrom(query)) : std::nullopt;
}

std::optional<Section> Store::fetchSection(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare("SELECT id, list_id, name, position FROM sections WHERE id = ?");
    query.bindValue(0, id);
    if (!query.exec()) {
        fail(query.lastError().text());
        m_broken = true;
        return std::nullopt;
    }
    return query.next() ? std::optional<Section>(sectionFrom(query)) : std::nullopt;
}

template <typename T>
bool Store::rebase(T &row, std::optional<T> &before, std::optional<T> (Store::*fetch)(qint64)) {
    if (m_broken)
        return false;
    if (!before)
        return true;
    const std::optional<T> current = (this->*fetch)(row.id);
    if (m_broken)
        return false;
    if (!current) {
        fail("It was deleted in another window or by a command, so the change was not saved.");
        m_broken = true;
        return false;
    }
    if (*current != *before) {
        row = rebased(*current, *before, row);
        before = current;
    }
    return true;
}

qint64 Store::put(Reminder r) {
    if (!m_open)
        return 0;
    std::optional<Reminder> before;
    if (const Reminder *existing = r.id ? reminder(r.id) : nullptr)
        before = *existing;
    if (before && *before == r)
        return r.id;

    begin();
    if (!rebase(r, before, &Store::fetchReminder))
        return commit() ? r.id : 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!r.createdAt)
        r.createdAt = now;
    r.modifiedAt = now;
    // A new date or time is a new moment to alert at, given it now.
    const bool moved = !before || before->due != r.due || before->dueMinutes != r.dueMinutes;
    if (moved || before->early != r.early)
        r.scheduledAt = now;
    if (before && moved)
        r.alertedDue = r.alertedEarly = r.snoozeUntil = 0;
    else if (before && before->early != r.early)
        r.alertedEarly = 0;

    if (write(r)) {
        m_reminders.insert(r.id, r);
        m_pending.reminders.append({r.id, before, r});
        // A completed reminder has no open subtasks: one that is open takes
        // its parent back to do.
        if (const Reminder *parent = r.completed ? nullptr : reminder(r.parentId); parent && parent->completed) {
            Reminder reopened = *parent;
            reopened.completed = false;
            reopened.completedAt = 0;
            put(reopened);
        }
    }
    return commit() ? r.id : 0;
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
    if (rebase(list, before, &Store::fetchList) && write(list)) {
        apply(list.id, std::optional<TaskList>(list));
        m_pending.lists.append({list.id, before, list});
    }
    return commit() ? list.id : 0;
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
    if (rebase(section, before, &Store::fetchSection) && write(section)) {
        apply(section.id, std::optional<Section>(section));
        m_pending.sections.append({section.id, before, section});
    }
    return commit() ? section.id : 0;
}

void Store::eraseReminder(qint64 id) {
    const Reminder *existing = reminder(id);
    if (!existing || !erase("reminders", id))
        return;
    m_pending.reminders.append({id, *existing, std::nullopt});
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
        if (section.listId != id || !erase("sections", section.id))
            continue;
        m_pending.sections.append({section.id, section, std::nullopt});
        apply(section.id, std::optional<Section>());
    }
    if (erase("lists", id)) {
        m_pending.lists.append({id, removed, std::nullopt});
        apply(id, std::optional<TaskList>());
    }
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
    if (erase("sections", id)) {
        m_pending.sections.append({id, removed, std::nullopt});
        apply(id, std::optional<Section>());
    }
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
        rule.repeats() && r.due.isValid() ? rule.upcoming(r.due, r.dueMinutes, now, r.repeatEnd) : QDate();

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
    return commit() ? next : QDate();
}

void Store::reopen(qint64 id) {
    const Reminder *existing = reminder(id);
    if (!m_open || !existing || !existing->completed)
        return;
    Reminder r = *existing;
    r.completed = false;
    r.completedAt = 0;
    put(r); // which reopens its parent too: an open subtask means that is not finished either
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

// Undo and redo put a reminder back as an edit made just now, so a moment
// that went by meanwhile is old news. Alerts already sent for the moment it
// has now stay sent; going back to another moment brings back what was
// known about that one, but not a snooze that ran out meanwhile.
void Store::restore(qint64 id, const std::optional<Reminder> &state) {
    const Reminder *found = reminder(id);
    const std::optional<Reminder> current = found ? std::optional<Reminder>(*found) : std::nullopt;
    if (!state) {
        if (!erase("reminders", id))
            return;
        m_reminders.remove(id);
        m_pending.reminders.append({id, current, std::nullopt});
        return;
    }
    Reminder r = *state;
    r.modifiedAt = QDateTime::currentSecsSinceEpoch();
    if (current && current->due == r.due && current->dueMinutes == r.dueMinutes) {
        r.alertedDue = current->alertedDue;
        r.alertedEarly = current->alertedEarly;
        r.snoozeUntil = current->snoozeUntil;
    } else if (r.snoozeUntil <= r.modifiedAt) {
        r.snoozeUntil = 0;
    }
    // Given back another moment, it was given it just now.
    if (current && current->due == r.due && current->dueMinutes == r.dueMinutes && current->early == r.early)
        r.scheduledAt = current->scheduledAt;
    else
        r.scheduledAt = r.modifiedAt;
    if (!write(r))
        return;
    m_reminders.insert(id, r);
    m_pending.reminders.append({id, current, r});
}

void Store::restore(qint64 id, const std::optional<TaskList> &state) {
    const TaskList *found = list(id);
    const std::optional<TaskList> current = found ? std::optional<TaskList>(*found) : std::nullopt;
    TaskList written = state.value_or(TaskList());
    if (state ? !write(written) : !erase("lists", id))
        return;
    apply(id, state);
    m_pending.lists.append({id, current, state});
}

void Store::restore(qint64 id, const std::optional<Section> &state) {
    const Section *found = section(id);
    const std::optional<Section> current = found ? std::optional<Section>(*found) : std::nullopt;
    Section written = state.value_or(Section());
    if (state ? !write(written) : !erase("sections", id))
        return;
    apply(id, state);
    m_pending.sections.append({id, current, state});
}

// Writes back one side of a step, under the lock lockFresh() took: what was
// there before it (undo), or what it made (redo). What it writes is noted
// in m_pending, so the step can go on to the other stack as it now stands.
bool Store::replay(const UndoEntry &entry, bool forward) {
    m_pending = UndoEntry();
    auto each = [this, forward](const auto &deltas) {
        if (forward)
            for (auto it = deltas.cbegin(); it != deltas.cend(); ++it)
                restore(it->id, it->after);
        else
            for (auto it = deltas.crbegin(); it != deltas.crend(); ++it)
                restore(it->id, it->before);
    };
    each(entry.lists);
    each(entry.sections);
    each(entry.reminders);
    return finish();
}

// The same changes the other way round: what redo takes back from an undo.
Store::UndoEntry Store::inverse(const UndoEntry &entry) {
    UndoEntry flipped;
    auto flip = [](const auto &deltas, auto &into) {
        for (auto it = deltas.crbegin(); it != deltas.crend(); ++it)
            into.append({it->id, it->after, it->before});
    };
    flip(entry.lists, flipped.lists);
    flip(entry.sections, flipped.sections);
    flip(entry.reminders, flipped.reminders);
    return flipped;
}

// Undo and redo write back what this process last saw, so they hold the
// write lock with memory matching the file. When another process changed
// reminders meanwhile, that shows first and nothing is held: undo waits to
// be asked again, with the steps that would write over the change gone.
bool Store::lockFresh() {
    for (int tries = 0; tries < 3; ++tries) {
        if (!run("BEGIN IMMEDIATE"))
            return false;
        if (dataVersion() == m_dataVersion)
            return true;
        m_db.rollback();
        if (reloadIfChanged())
            return false;
    }
    return false;
}

void Store::undo() {
    if (!m_open || m_depth > 0 || m_undo.isEmpty() || !lockFresh())
        return;
    const UndoEntry entry = m_undo.last();
    if (!replay(entry, false))
        return;
    m_undo.removeLast();
    m_redo.append(inverse(m_pending));
    m_pending = UndoEntry();
    // Whatever comes next is a step of its own, even in the same field.
    sealUndo();
    emit undoChanged();
    emit changed();
}

void Store::redo() {
    if (!m_open || m_depth > 0 || m_redo.isEmpty() || !lockFresh())
        return;
    const UndoEntry entry = m_redo.last();
    if (!replay(entry, true))
        return;
    m_redo.removeLast();
    m_undo.append(m_pending); // with no merge key, so nothing merges into it
    m_pending = UndoEntry();
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
    if (!m_open || m_depth > 0 || m_undo.isEmpty() || m_undo.last().mergeKey != mergeKey || !lockFresh())
        return;
    const UndoEntry entry = m_undo.last();
    if (!replay(entry, false))
        return;
    m_undo.removeLast();
    m_pending = UndoEntry();
    sealUndo();
    emit undoChanged();
    emit changed();
}

void Store::setAlerted(qint64 id, qint64 due, qint64 early, qint64 snoozeUntil) {
    const auto found = m_reminders.find(id);
    if (!m_open || found == m_reminders.end())
        return;
    // Memory takes the marks even when the disk does not: an alert that
    // could not be marked would otherwise go out again every minute.
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

struct Store::Rows {
    QSet<qint64> lists, sections, reminders;

    bool isEmpty() const { return lists.isEmpty() && sections.isEmpty() && reminders.isEmpty(); }
    bool meets(const Rows &other) const {
        return lists.intersects(other.lists) || sections.intersects(other.sections) ||
               reminders.intersects(other.reminders);
    }
    void add(const Rows &other) {
        lists.unite(other.lists);
        sections.unite(other.sections);
        reminders.unite(other.reminders);
    }
    // What a reminder sits in: its list, and its section and parent if any.
    void holdFor(const Reminder &r) {
        lists.insert(r.listId);
        if (r.sectionId)
            sections.insert(r.sectionId);
        if (r.parentId)
            reminders.insert(r.parentId);
    }
};

bool Store::reloadIfChanged() {
    if (!m_open || m_depth > 0 || dataVersion() == m_dataVersion)
        return false;
    const auto lists = m_lists;
    const auto sections = m_sections;
    const auto reminders = m_reminders;
    if (!load())
        return false;

    // What another process changed, which of those rows it deleted, and
    // what its reminders and sections now sit in.
    Rows touched, gone, held;
    auto compare = [](const auto &before, const auto &after, QSet<qint64> &changed, QSet<qint64> &deleted) {
        for (const auto &row : before)
            if (std::none_of(after.cbegin(), after.cend(), [&row](const auto &now) { return now.id == row.id; }))
                deleted.insert(row.id);
        for (const auto &row : after)
            if (std::find(before.cbegin(), before.cend(), row) == before.cend())
                changed.insert(row.id);
        changed.unite(deleted);
    };
    compare(lists, m_lists, touched.lists, gone.lists);
    compare(sections, m_sections, touched.sections, gone.sections);
    for (const Section &section : std::as_const(m_sections))
        if (touched.sections.contains(section.id))
            held.lists.insert(section.listId);
    for (auto it = m_reminders.cbegin(); it != m_reminders.cend(); ++it) {
        const auto old = reminders.constFind(it.key());
        if (old != reminders.cend() && old->sameContent(*it))
            continue;
        touched.reminders.insert(it.key());
        held.holdFor(*it);
    }
    for (auto it = reminders.cbegin(); it != reminders.cend(); ++it)
        if (!m_reminders.contains(it.key())) {
            touched.reminders.insert(it.key());
            gone.reminders.insert(it.key());
        }
    // Another process only noting which alerts it sent is no reason to
    // disturb the window or forget what can be undone.
    if (touched.isEmpty())
        return false;
    forget(touched, held, gone);
    emit changed();
    return true;
}

// A step that would write over another process's changes, or pull a list,
// section or parent out from under its reminders, can no longer be undone
// or redone, and goes; every other step stays. What a step that goes wrote
// then stays as it is, as far as the steps beneath it are concerned.
void Store::forget(const Rows &touched, const Rows &held, const Rows &gone) {
    const qsizetype top = m_undo.size() - 1;
    bool dropped = false, topGone = false;
    for (QVector<UndoEntry> *stack : {&m_undo, &m_redo}) {
        Rows fixed = touched, holding = held, missing = gone;
        for (qsizetype i = stack->size() - 1; i >= 0; --i) {
            // The rows the step writes, what its states sit in, and the rows it adds or deletes.
            Rows writes, holds, makes;
            auto note = [](const auto &deltas, QSet<qint64> &written, QSet<qint64> &made) {
                for (const auto &delta : deltas) {
                    written.insert(delta.id);
                    if (!delta.before || !delta.after)
                        made.insert(delta.id);
                }
            };
            const UndoEntry &step = stack->at(i);
            note(step.lists, writes.lists, makes.lists);
            note(step.sections, writes.sections, makes.sections);
            note(step.reminders, writes.reminders, makes.reminders);
            for (const auto &delta : step.sections)
                for (const auto *state : {&delta.before, &delta.after})
                    if (*state)
                        holds.lists.insert((*state)->listId);
            for (const auto &delta : step.reminders)
                for (const auto *state : {&delta.before, &delta.after})
                    if (*state)
                        holds.holdFor(**state);
            if (!writes.meets(fixed) && !writes.meets(holding) && !holds.meets(missing))
                continue;
            fixed.add(writes);
            holding.add(holds);
            missing.add(makes);
            stack->removeAt(i);
            dropped = true;
            topGone = topGone || (stack == &m_undo && i == top);
        }
    }
    // An older step is on top now; the next edit starts a step of its own.
    if (topGone)
        sealUndo();
    if (dropped)
        emit undoChanged();
}
