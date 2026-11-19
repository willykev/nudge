#include "store.h"

#include "recurrence.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QStandardPaths>
#include <QThread>
#include <QVariant>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <tuple>
#include <utility>
#include <sys/stat.h>

namespace {
QString isoDate(const QDate &date) { return date.isValid() ? date.toString(Qt::ISODate) : QString(); }
QDate fromIso(const QString &text) { return text.isEmpty() ? QDate() : QDate::fromString(text, Qt::ISODate); }
QVariant idOrNull(qint64 id) { return id ? QVariant(id) : QVariant(QMetaType::fromType<qint64>()); }
QString normalPath() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/nudge/nudge.db"; }

QString physicalPath(const QString &path) {
    QFileInfo file(path);
    QSet<QString> visited;
    while (file.isSymLink() && !visited.contains(file.absoluteFilePath())) {
        visited.insert(file.absoluteFilePath());
        file.setFile(file.symLinkTarget());
    }
    const QString directory = file.dir().canonicalPath();
    return QDir::cleanPath(directory.isEmpty() ? file.absoluteFilePath() : QDir(directory).filePath(file.fileName()));
}

bool sameFile(const QString &a, const QString &b) {
    if (a == b || physicalPath(a) == physicalPath(b))
        return true;
    struct stat first{}, second{};
    return ::stat(QFile::encodeName(a).constData(), &first) == 0 &&
           ::stat(QFile::encodeName(b).constData(), &second) == 0 &&
           first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

const char *const reminderColumns =
    "id, list_id, section_id, parent_id, title, notes, url, due, due_minutes, repeat,"
    "repeat_end, early, priority, flagged, tags, completed, completed_at, created_at,"
    "modified_at, position, alerted_due, alerted_early, snooze_until, scheduled_at, late_fold, early_scheduled_at, draft";

constexpr int busyTimeoutMs = 4000;

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
    r.lateFold = query.value(i++).toBool();
    r.earlyScheduledAt = query.value(i++).toLongLong();
    r.draft = query.value(i++).toBool();
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
    if (after.due != before.due || after.dueMinutes != before.dueMinutes || after.lateFold != before.lateFold) {
        merged.due = after.due;
        merged.dueMinutes = after.dueMinutes;
        merged.lateFold = after.lateFold;
    }
    TAKE2(repeat, repeatEnd);
    TAKE(early);
    TAKE(priority);
    TAKE(flagged);
    TAKE(tags);
    TAKE2(completed, completedAt);
    TAKE(draft);
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
    void holdFor(const Reminder &r) {
        lists.insert(r.listId);
        if (r.sectionId)
            sections.insert(r.sectionId);
        if (r.parentId)
            reminders.insert(r.parentId);
    }
};

bool Reminder::sameContent(const Reminder &other) const {
    auto fields = [](const Reminder &r) {
        return std::tie(r.id, r.listId, r.sectionId, r.parentId, r.title, r.notes, r.url, r.due, r.dueMinutes,
                        r.lateFold, r.repeat, r.repeatEnd, r.early, r.priority, r.flagged, r.tags, r.completed, r.draft,
                        r.completedAt, r.createdAt, r.modifiedAt, r.scheduledAt, r.earlyScheduledAt, r.position);
    };
    return fields(*this) == fields(other);
}

bool Reminder::hasContent() const {
    return !title.trimmed().isEmpty() || !notes.isEmpty() || !url.isEmpty() || due.isValid() ||
           !repeat.isEmpty() || early || priority || flagged || !tags.isEmpty() || completed;
}

qint64 Reminder::dueAt(const QTimeZone &zone) const {
    if (!due.isValid())
        return 0;
    const int minutes = dueMinutes >= 0 ? dueMinutes : allDayAlertMinutes;
    const auto resolve = lateFold ? QDateTime::TransitionResolution::PreferAfter
                                  : QDateTime::TransitionResolution::LegacyBehavior;
    return QDateTime(due, QTime(minutes / 60, minutes % 60), zone, resolve).toSecsSinceEpoch();
}

QString Store::defaultPath() {
    const QString override = qEnvironmentVariable("NUDGE_DB");
    if (!override.isEmpty())
        return override;
    return normalPath();
}

bool Store::isDatabaseFile(const QString &database, const QString &path) {
    const QString physical = physicalPath(database);
    for (const QString &suffix : {QString(), QString("-wal"), QString("-shm"), QString("-journal")})
        if (sameFile(database + suffix, path) || (database != physical && sameFile(physical + suffix, path)))
            return true;
    return false;
}

QString Store::copyForPicture(const QString &source, const QString &destination) {
    if (!QFileInfo::exists(source))
        return {};
    const QString name = "nudge-picture";
    QString error;
    {
        auto reader = QSqlDatabase::addDatabase("QSQLITE", name);
        reader.setDatabaseName(source);
        reader.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=4000");
        if (!reader.open()) {
            error = reader.lastError().text();
        } else {
            QSqlQuery copy(reader);
            if (!copy.prepare("VACUUM INTO ?")) {
                error = copy.lastError().text();
            } else {
                copy.addBindValue(destination);
                if (!copy.exec())
                    error = copy.lastError().text();
            }
        }
    }
    QSqlDatabase::removeDatabase(name);
    return error;
}

Store::Store(const QString &path, QObject *parent) : QObject(parent) {
    static std::atomic_int connections = 0;
    m_connection = QString("nudge-%1").arg(++connections);
    m_path = path.isEmpty() ? defaultPath() : path;

    m_reload.setSingleShot(true);
    m_reload.setInterval(60);
    connect(&m_reload, &QTimer::timeout, this, [this] {
        arm();
        reloadIfChanged();
    });
    const auto refreshSoon = [this] {
        if (!m_reload.isActive())
            m_reload.start();
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, refreshSoon);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, refreshSoon);

    const QString folder = QFileInfo(m_path).absolutePath();
    const bool newFolder = !QFileInfo::exists(folder);
    if (!QDir().mkpath(folder)) {
        fail(QString("Could not create %1").arg(folder));
        return;
    }
    // What is written down here is the person's own: the folder Nudge makes,
    // or its own one, is theirs alone, and so is the file.
    if (newFolder || QFileInfo(normalPath()).absolutePath() == folder)
        QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    keepPrivate();
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_db.setDatabaseName(m_path);
    // The window, the command line, and the background alerts may all write.
    m_db.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(busyTimeoutMs));
    if (!m_db.open()) {
        fail(QString("Could not open %1: %2").arg(m_path, m_db.lastError().text()));
        return;
    }

    const bool ready =
        // Every change is on the disk before it shows as saved: a laptop can
        // lose power, and a reminder that vanished is worse than a millisecond.
        enableWal() && run("PRAGMA synchronous=FULL") &&
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
            "scheduled_at INTEGER NOT NULL DEFAULT 0, late_fold INTEGER NOT NULL DEFAULT 0,"
            "early_scheduled_at INTEGER NOT NULL DEFAULT 0, draft INTEGER NOT NULL DEFAULT 0)") &&
        // Files from Nudge 0.1.3 and earlier: until a reminder is given a
        // moment anew, it was last given one when it was last changed.
        migrate() &&
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

bool Store::enableWal() {
    const QDeadlineTimer deadline(busyTimeoutMs);
    for (;;) {
        QSqlError error;
        {
            QSqlQuery query(m_db);
            if (query.exec("PRAGMA journal_mode=WAL") && query.next())
                return true;
            error = query.lastError();
        }
        // Two connections opening a new file can both hold a read lock while
        // asking for the exclusive lock. SQLite skips its busy wait then.
        constexpr int sqliteBusy = 5;
        constexpr int sqliteLocked = 6;
        const int code = error.nativeErrorCode().toInt() & 0xff;
        if (deadline.hasExpired() || (code != sqliteBusy && code != sqliteLocked)) {
            fail(error.text());
            return false;
        }
        QThread::msleep(20);
    }
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

bool Store::migrate() {
    if (hasColumn("reminders", "scheduled_at") && hasColumn("reminders", "late_fold") &&
        hasColumn("reminders", "early_scheduled_at") && hasColumn("reminders", "draft"))
        return true;
    if (!run("BEGIN IMMEDIATE"))
        return false;
    // Another process may have added the column while this one waited.
    if ((hasColumn("reminders", "scheduled_at") ||
         (run("ALTER TABLE reminders ADD COLUMN scheduled_at INTEGER NOT NULL DEFAULT 0") &&
          run("UPDATE reminders SET scheduled_at = modified_at"))) &&
        (hasColumn("reminders", "late_fold") ||
         run("ALTER TABLE reminders ADD COLUMN late_fold INTEGER NOT NULL DEFAULT 0")) &&
        (hasColumn("reminders", "early_scheduled_at") ||
         run("ALTER TABLE reminders ADD COLUMN early_scheduled_at INTEGER NOT NULL DEFAULT 0")) &&
        (hasColumn("reminders", "draft") || run("ALTER TABLE reminders ADD COLUMN draft INTEGER NOT NULL DEFAULT 0")) &&
        run("COMMIT"))
        return true;
    m_db.rollback();
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
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
            .arg(QLatin1String(reminderColumns)));
    m_putList = QSqlQuery(m_db);
    m_putList.prepare("INSERT OR REPLACE INTO lists (id, name, color, position, sort) VALUES (?, ?, ?, ?, ?)");
    m_putSection = QSqlQuery(m_db);
    m_putSection.prepare("INSERT OR REPLACE INTO sections (id, list_id, name, position) VALUES (?, ?, ?, ?)");
    m_setAlerted = QSqlQuery(m_db);
    m_setAlerted.prepare("UPDATE reminders SET alerted_due = ?, alerted_early = ?, snooze_until = ? "
                        "WHERE id = ? AND completed = 0 AND due = ? AND due_minutes = ? AND late_fold = ? AND early = ?");
}

// Everything is read in one read transaction, so it is one moment of the
// file, and the version noted is that moment's: a commit landing meanwhile
// is still news next time. Memory stays as it was when the file cannot be read.
bool Store::load(bool locked) {
    QVector<TaskList> lists;
    QVector<Section> sections;
    QHash<qint64, Reminder> reminders;
    const bool reading = locked || m_db.transaction();
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
    if (reading && !locked)
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
    Tx tx(this, {}, Latest);
    if (!tx) {
        m_recording = true;
        return;
    }
    for (const qint64 id : std::as_const(parents)) {
        if (const Reminder *parent = reminder(id); parent && parent->completed)
            reopen(id);
    }
    tx.commit();
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
    Tx tx(this, {}, Latest);
    if (!tx)
        return 0;
    if (!m_lists.isEmpty())
        return m_lists.first().id;
    TaskList list;
    list.name = QCoreApplication::translate("Store", "Reminders");
    list.color = "blue";
    list.position = 1;
    // The default list outlives undo of the first reminder.
    if (!write(list))
        return 0;
    apply(list.id, list);
    m_externalChange = true;
    // Keep the insert in the rollback journal, without adding it to undo.
    m_pending.lists.append({list.id, std::nullopt, list});
    m_pending.lists.last().keepOnUndo = true;
    return tx.commit() ? list.id : 0;
}

bool Store::Tx::commit() {
    return std::exchange(m_store, nullptr)->commit();
}

bool Store::begin(const QString &mergeKey, Snapshot snapshot) {
    if (!m_open)
        return false;
    if (m_depth++ == 0) {
        m_pending = UndoEntry();
        m_pending.mergeKey = mergeKey;
        // Outside a transaction each write would stand on its own, so none is
        // tried. The write lock is taken at once: a row is read again before
        // it is written (see rebase()), and nothing may change it in between.
        if (!run("BEGIN IMMEDIATE"))
            m_broken = true;
        m_current = !m_broken && dataVersion() == m_dataVersion;
    }
    if (!m_broken && snapshot == Latest && !m_current) {
        const auto changed = readChanges(true);
        m_broken = !changed.has_value();
        m_current = !m_broken;
        m_externalChange |= changed.value_or(false);
    }
    return !m_broken;
}

bool Store::commit() {
    if (!m_open || m_depth == 0)
        return false;
    if (--m_depth > 0)
        return !m_broken;
    const bool external = std::exchange(m_externalChange, false);
    bool undoChanged = std::exchange(m_undoChanged, false);
    if (!finish()) {
        if (undoChanged)
            emit this->undoChanged();
        return false;
    }
    m_pending.lists.removeIf([](const auto &delta) { return delta.keepOnUndo; });
    const bool written = !m_pending.isEmpty();
    if (written && m_recording) {
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
        undoChanged = true;
    }
    m_pending = UndoEntry();
    if (undoChanged)
        emit this->undoChanged();
    if (written || external)
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
    query.bindValue(i++, r.lateFold);
    query.bindValue(i++, r.earlyScheduledAt);
    query.bindValue(i++, r.draft);
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
    if (!row.id)
        return true;
    const std::optional<T> current = m_current ? before : (this->*fetch)(row.id);
    if (m_broken)
        return false;
    if (!before || !current) {
        fail(tr("It was deleted in another window or by a command, so the change was not saved."));
        m_broken = true;
        return false;
    }
    if (*current != *before) {
        // Once the merged row is in memory, a later reload cannot tell
        // which part came from another writer. Retire older undo steps now.
        Rows touched, held;
        if constexpr (std::is_same_v<T, Reminder>) {
            if (!current->sameContent(*before)) {
                touched.reminders.insert(row.id);
                held.holdFor(*current);
            }
        } else if constexpr (std::is_same_v<T, TaskList>) {
            touched.lists.insert(row.id);
        } else {
            touched.sections.insert(row.id);
            held.lists.insert(current->listId);
        }
        if (!touched.isEmpty())
            forget(touched, held, {});
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
    if (!r.title.trimmed().isEmpty())
        r.draft = false;
    if (r.repeat == "weekdays" && (!before || r.due != before->due || r.repeat != before->repeat))
        r.due = Repeat::firstWorkday(r.due);
    if (!r.due.isValid() && !r.repeat.isEmpty() && (!before || before->due.isValid() || before->repeat != r.repeat)) {
        fail(tr("A repeating reminder needs a date."));
        m_broken = true;
        return commit() ? r.id : 0;
    }
    const bool newHome = !before || before->listId != r.listId || before->sectionId != r.sectionId ||
                         before->parentId != r.parentId;
    if (newHome) {
        const bool hasList = m_current ? list(r.listId) != nullptr : fetchList(r.listId).has_value();
        const auto folder = r.sectionId ? (m_current && section(r.sectionId)
                                               ? std::optional(*section(r.sectionId)) : fetchSection(r.sectionId))
                                        : std::optional<Section>();
        const auto parent = r.parentId ? (m_current && reminder(r.parentId)
                                             ? std::optional(*reminder(r.parentId)) : fetchReminder(r.parentId))
                                      : std::optional<Reminder>();
        if (!hasList || (r.sectionId && (!folder || folder->listId != r.listId)) ||
            (r.parentId && (!parent || parent->listId != r.listId || parent->parentId || r.parentId == r.id))) {
            if (!m_broken)
                fail(tr("The reminder’s list, section, or parent has changed."));
            m_broken = true;
            return commit() ? r.id : 0;
        }
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!r.createdAt)
        r.createdAt = now;
    r.modifiedAt = now;
    // A new date or time is a new moment to alert at, given it now.
    const bool moved = !before || before->due != r.due || before->dueMinutes != r.dueMinutes ||
                       before->lateFold != r.lateFold;
    if (moved)
        r.scheduledAt = now;
    if (moved || before->early != r.early)
        r.earlyScheduledAt = now;
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
    if (rebase(section, before, &Store::fetchSection) &&
        (!before || before->listId != section.listId) &&
        !(m_current ? list(section.listId) != nullptr : fetchList(section.listId).has_value())) {
        if (!m_broken)
            fail(tr("The reminder’s list, section, or parent has changed."));
        m_broken = true;
    }
    if (write(section)) {
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
    Tx tx(this, {}, Latest);
    if (!tx || !reminder(id))
        return;
    for (qint64 child : childrenOf(id))
        eraseReminder(child);
    eraseReminder(id);
}

qsizetype Store::removeReminders(const QVector<qint64> &ids) {
    Tx tx(this, {}, Latest);
    if (!tx)
        return 0;
    QSet<qint64> removing(ids.cbegin(), ids.cend());
    for (const Reminder &r : std::as_const(m_reminders)) {
        if (removing.contains(r.id))
            continue;
        // A child outside the selection keeps its ancestors attached.
        for (qint64 parent = r.parentId; parent && removing.remove(parent);) {
            const Reminder *row = reminder(parent);
            parent = row ? row->parentId : 0;
        }
    }
    qsizetype count = 0;
    for (qint64 id : ids) {
        if (removing.remove(id) && reminder(id)) {
            eraseReminder(id);
            ++count;
        }
    }
    return tx.commit() ? count : 0;
}

void Store::removeEmptyDraft(qint64 id) {
    Tx tx(this, {}, Latest);
    if (const Reminder *r = tx ? reminder(id) : nullptr;
        r && !r->hasContent() && childrenOf(id).isEmpty())
        eraseReminder(id);
}

void Store::cleanUpDrafts(qint64 olderThan) {
    // Check again under the lock in case another window has filled one in.
    auto empty = [olderThan](const Reminder &r) { return !r.hasContent() && r.modifiedAt < olderThan; };
    if (std::none_of(m_reminders.cbegin(), m_reminders.cend(), empty))
        return;
    Tx tx(this, {}, Latest);
    if (!tx)
        return;
    QVector<qint64> blank;
    for (const Reminder &r : std::as_const(m_reminders))
        if (empty(r))
            blank.append(r.id);
    const bool recording = std::exchange(m_recording, false);
    const auto removed = removeReminders(blank);
    const bool saved = tx.commit();
    m_recording = recording;
    if (removed && saved)
        clearUndo();
}

void Store::removeList(qint64 id) {
    Tx tx(this, {}, Latest);
    const TaskList *existing = tx ? list(id) : nullptr;
    if (!existing)
        return;
    const TaskList removed = *existing; // the vector it points into is about to change
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
}

void Store::removeSection(qint64 id) {
    Tx tx(this, {}, Latest);
    const Section *existing = tx ? section(id) : nullptr;
    if (!existing)
        return;
    const Section removed = *existing;
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
}

QDate Store::complete(qint64 id, const QDateTime &now, bool *completed) {
    if (completed)
        *completed = false;
    const Reminder *existing = reminder(id);
    if (!m_open || !existing || existing->completed || existing->draft)
        return {};
    const Reminder offered = *existing;
    Tx tx(this, {}, Latest);
    existing = tx ? reminder(id) : nullptr;
    if (!existing || existing->completed || existing->draft || existing->due != offered.due ||
        existing->dueMinutes != offered.dueMinutes || existing->lateFold != offered.lateFold)
        return {};
    Reminder r = *existing;
    const qint64 seconds = now.toSecsSinceEpoch();
    auto following = [&now](const Reminder &row) {
        return Repeat::parse(row.repeat).upcoming(row.due, row.dueMinutes, now, row.repeatEnd);
    };
    auto advance = [this, seconds](Reminder row, const QDate &date) {
        if (date.isValid()) {
            Reminder history = row;
            history.id = 0;
            // This occurrence stays in Completed when the parent repeats.
            history.parentId = 0;
            history.repeat.clear();
            history.repeatEnd = {};
            history.completed = true;
            history.completedAt = seconds;
            put(history);
            row.due = date;
            row.lateFold = false;
        } else {
            row.completed = true;
            row.completedAt = seconds;
        }
        put(row);
    };
    const QDate next = following(r);

    const QVector<qint64> children = childrenOf(id);
    advance(r, next);
    if (next.isValid()) {
        // Before history was detached, an occurrence could still be under
        // this parent. It shares the series' title and creation timestamp.
        QHash<QPair<QString, qint64>, QDate> liveRepeats;
        for (qint64 childId : children) {
            const Reminder &child = *reminder(childId);
            if (!child.repeat.isEmpty())
                liveRepeats.insert({child.title, child.createdAt}, child.due);
        }
        for (qint64 childId : children) {
            Reminder child = *reminder(childId);
            if (!child.completed || !child.repeat.isEmpty())
                continue;
            const QDate live = liveRepeats.value({child.title, child.createdAt});
            if (live.isValid() && child.due.isValid() && child.due < live)
                continue;
            child.completed = false;
            child.completedAt = 0;
            put(child);
        }
    } else {
        for (qint64 childId : children) {
            Reminder child = *reminder(childId);
            if (child.completed)
                continue;
            const QDate childNext = following(child);
            if (childNext.isValid()) {
                // The child still has something to do after its parent ends.
                // Keep its completed occurrence with the parent.
                Reminder history = child;
                history.id = 0;
                history.repeat.clear();
                history.repeatEnd = {};
                history.completed = true;
                history.completedAt = seconds;
                put(history);
                child.parentId = 0;
                child.position = r.position;
                child.due = childNext;
                child.lateFold = false;
                put(child);
            } else {
                advance(child, {});
            }
        }
    }
    const bool saved = tx.commit();
    if (completed)
        *completed = saved;
    return saved ? next : QDate();
}

bool Store::reopen(qint64 id) {
    Tx tx(this, {}, Latest);
    const Reminder *existing = tx ? reminder(id) : nullptr;
    if (!existing || !existing->completed)
        return false;
    Reminder r = *existing;
    r.completed = false;
    r.completedAt = 0;
    const bool saved = put(r); // an open subtask reopens its parent too
    return tx.commit() && saved;
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
    const bool sameMoment = current && current->due == r.due && current->dueMinutes == r.dueMinutes &&
                            current->lateFold == r.lateFold;
    if (sameMoment) {
        r.alertedDue = current->alertedDue;
        if (current->early == r.early)
            r.alertedEarly = current->alertedEarly;
        r.snoozeUntil = current->snoozeUntil;
    } else if (r.snoozeUntil <= r.modifiedAt) {
        r.snoozeUntil = 0;
    }
    // Given back another moment, it was given it just now.
    r.scheduledAt = sameMoment ? current->scheduledSince() : r.modifiedAt;
    r.earlyScheduledAt = sameMoment && current->early == r.early ? current->earlySince() : r.modifiedAt;
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
    m_setAlerted.bindValue(4, found->due.isValid() ? isoDate(found->due) : QStringLiteral(""));
    m_setAlerted.bindValue(5, found->dueMinutes);
    m_setAlerted.bindValue(6, found->lateFold);
    m_setAlerted.bindValue(7, found->early);
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
    if (!m_open || m_depth > 0)
        return false;
    if (!readChanges().value_or(false))
        return false;
    emit changed();
    return true;
}

std::optional<bool> Store::readChanges(bool locked) {
    if (!locked && dataVersion() == m_dataVersion)
        return false;
    const auto lists = m_lists;
    const auto sections = m_sections;
    const auto reminders = m_reminders;
    if (!load(locked))
        return std::nullopt;

    // What another process changed, which of those rows it deleted, and
    // what its reminders and sections now sit in.
    Rows touched, gone, held;
    auto compare = [](const auto &before, const auto &after, QSet<qint64> &changed, QSet<qint64> &deleted) {
        if (before == after)
            return;
        QHash<qint64, const typename std::decay_t<decltype(before)>::value_type *> old;
        for (const auto &row : before)
            old.insert(row.id, &row);
        for (const auto &row : after) {
            const auto *was = old.take(row.id);
            if (!was || *was != row)
                changed.insert(row.id);
        }
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            deleted.insert(it.key());
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
    if (dropped && m_depth)
        m_undoChanged = true;
    else if (dropped)
        emit undoChanged();
}
