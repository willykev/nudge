#pragma once

#include <QDate>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTimeZone>
#include <QTimer>
#include <QVector>
#include <functional>
#include <optional>

struct TaskList {
    qint64 id = 0;
    QString name;
    QString color;
    double position = 0;
    QString sort = "manual"; // manual, due, priority, title, created

    bool operator==(const TaskList &) const = default;
};

struct Section {
    qint64 id = 0;
    qint64 listId = 0;
    QString name;
    double position = 0;

    bool operator==(const Section &) const = default;
};

struct Reminder {
    // A reminder with a date but no time alerts at nine in the morning.
    static constexpr int allDayAlertMinutes = 9 * 60;

    qint64 id = 0;
    qint64 listId = 0;
    qint64 sectionId = 0; // 0 when it sits above the list's sections
    qint64 parentId = 0;  // 0 unless it is a subtask
    QString title, notes, url;
    QDate due;           // invalid when there is no date
    int dueMinutes = -1; // minutes after midnight, -1 for all day
    bool lateFold = false; // the later instant when the clock shows this minute twice
    QString repeat;      // see recurrence.h
    QDate repeatEnd;
    int early = 0;       // minutes before due for an extra alert, 0 for none
    int priority = 0;    // 0 none, 1 low, 2 medium, 3 high
    bool flagged = false;
    QStringList tags;
    bool completed = false;
    bool draft = false; // still being written in the window that started it
    qint64 completedAt = 0; // seconds since the epoch, like the two below
    qint64 createdAt = 0;
    qint64 modifiedAt = 0;
    // The due moment and early lead can be set independently. A moment that
    // was already past when it was assigned is left quiet.
    qint64 scheduledAt = 0;
    qint64 earlyScheduledAt = 0;
    double position = 0;
    // Alert bookkeeping: the moments already announced, and a snooze.
    qint64 alertedDue = 0, alertedEarly = 0, snoozeUntil = 0;

    bool operator==(const Reminder &) const = default;
    bool hasText() const; // title, notes, or link entered for a draft
    bool hasContent() const;
    bool sameContent(const Reminder &other) const; // ignores the bookkeeping
    struct Occurrence {
        QDate date;
        int minutes = -1;
        bool lateFold = false;
        bool operator==(const Occurrence &) const = default;
    };
    Occurrence occurrence() const { return {due, dueMinutes, lateFold}; }
    qint64 dueAt(const QTimeZone &zone = QTimeZone::LocalTime) const; // 0 without a date
    qint64 scheduledSince() const { return scheduledAt ? scheduledAt : modifiedAt; }
    qint64 earlySince() const { return earlyScheduledAt ? earlyScheduledAt : scheduledSince(); }
};

// Every list, section, and reminder, held in memory and written through to
// SQLite as it changes. Changes are grouped into transactions that undo and
// redo as one step, and other processes (the command line, the background
// alerts) share the same file.
class Store : public QObject {
    Q_OBJECT

public:
    enum Snapshot { Cached, Latest };

    // Groups the changes made while it is alive into one undo step.
    class Tx {
    public:
        explicit Tx(Store *store, const QString &mergeKey = {}, Snapshot snapshot = Cached)
            : m_store(store), m_ready(store->begin(mergeKey, snapshot)) {}
        ~Tx() { if (m_store) m_store->commit(); }
        Tx(const Tx &) = delete;
        Tx &operator=(const Tx &) = delete;
        explicit operator bool() const { return m_ready; }
        bool commit();

    private:
        Store *m_store;
        bool m_ready;
    };

    explicit Store(const QString &path = {}, QObject *parent = nullptr);
    ~Store() override;

    static QString defaultPath();
    static bool isDatabaseFile(const QString &database, const QString &path);
    static QString copyForPicture(const QString &source, const QString &destination);
    bool isOpen() const { return m_open; }
    QString error() const { return m_error; }
    QString path() const { return m_path; }

    const QVector<TaskList> &lists() const { return m_lists; }
    const QVector<Section> &sections() const { return m_sections; }
    const QHash<qint64, Reminder> &reminders() const { return m_reminders; }
    const TaskList *list(qint64 id) const;
    const Section *section(qint64 id) const;
    const Reminder *reminder(qint64 id) const;
    QVector<qint64> childrenOf(qint64 id) const;
    double endPosition(qint64 listId) const;
    qint64 defaultListId();

    // A transaction reaches the disk whole or not at all. When a write in it
    // fails, failed() says why, the rest is skipped, and memory and undo are
    // left as they were; commit() then returns false.
    bool begin(const QString &mergeKey = {}, Snapshot snapshot = Cached);
    bool commit();
    // Each returns the id, or 0 when it could not be saved. An open reminder
    // reopens a completed parent, in the same step.
    // An edit that moves a reminder uses the normal clock time unless an
    // entry explicitly chose the later fold for the new time.
    qint64 put(Reminder reminder, bool keepFold = false);
    qint64 put(TaskList list);
    qint64 put(Section section);
    void removeReminder(qint64 id); // and its subtasks
    qsizetype removeReminders(const QVector<qint64> &ids); // keep a parent if any child stays
    void removeEmptyDraft(qint64 id);
    void cleanUpDrafts(qint64 olderThan);
    void removeList(qint64 id);     // and everything in it
    void removeSection(qint64 id);  // its reminders stay in the list

    // Completing a repeating reminder leaves a completed copy behind and
    // moves the reminder to its next date, which is returned.
    QDate complete(qint64 id, const QDateTime &now, bool *completed = nullptr);
    bool reopen(qint64 id); // and its parent, if that was completed

    // Undo and redo leave alone what another process has changed since:
    // the steps that would write over it are forgotten, and the rest stay.
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();
    void sealUndo();                            // later changes start a new step
    void clearUndo();                           // nothing done so far can be undone
    void discardLast(const QString &mergeKey);  // take back a step for good

    void setAlerted(qint64 id, qint64 due, qint64 early, qint64 snoozeUntil);
    QString meta(const QString &key) const;
    void setMeta(const QString &key, const QString &value);

    void watch(); // reload when another process changes the file
    bool reloadIfChanged();

    // For tests: runs part way through reading the file, so another
    // connection can change it at just that moment.
    void setLoadHook(std::function<void()> hook) { m_loadHook = std::move(hook); }

signals:
    void changed();
    void undoChanged();
    void failed(const QString &message);

private:
    template <typename T> struct Delta {
        qint64 id = 0;
        std::optional<T> before, after;
        bool keepOnUndo = false;
    };
    struct UndoEntry {
        QString mergeKey;
        QVector<Delta<TaskList>> lists;
        QVector<Delta<Section>> sections;
        QVector<Delta<Reminder>> reminders;
        bool isEmpty() const { return lists.isEmpty() && sections.isEmpty() && reminders.isEmpty(); }
    };

    struct Rows; // by table

    bool run(const QString &sql);
    bool enableWal();
    void fail(const QString &message);
    void prepare();
    bool load(bool locked = false);
    std::optional<bool> readChanges(bool locked = false);
    bool migrate();
    void repair();
    qint64 pragma(const char *name);
    qint64 dataVersion();
    void arm();
    std::optional<Reminder> fetchReminder(qint64 id);
    std::optional<TaskList> fetchList(qint64 id);
    std::optional<Section> fetchSection(qint64 id);
    // Before a row this process holds is written over, it is read again from
    // the file, inside the transaction that will write it. Gone, it was
    // deleted elsewhere and is not made again: the whole step fails. Changed
    // elsewhere, `row` keeps what this process changed of `before` and takes
    // the rest as it now is, and `before` becomes the row as found.
    template <typename T> bool rebase(T &row, std::optional<T> &before, std::optional<T> (Store::*fetch)(qint64));
    bool hasColumn(const QString &table, const QString &column);
    void keepPrivate();
    bool write(Reminder &reminder);
    bool write(TaskList &list);
    bool write(Section &section);
    bool erase(const char *table, qint64 id);
    void eraseReminder(qint64 id);
    void apply(qint64 id, const std::optional<TaskList> &state);
    void apply(qint64 id, const std::optional<Section> &state);
    bool finish();
    void revert(const UndoEntry &entry);
    bool lockFresh();
    bool replay(const UndoEntry &entry, bool forward);
    void restore(qint64 id, const std::optional<Reminder> &state);
    void restore(qint64 id, const std::optional<TaskList> &state);
    void restore(qint64 id, const std::optional<Section> &state);
    static UndoEntry inverse(const UndoEntry &entry);
    void forget(const Rows &touched, const Rows &held, const Rows &gone);
    void sortLists();

    QString m_path, m_connection, m_error;
    QSqlDatabase m_db;
    QSqlQuery m_putReminder, m_putList, m_putSection, m_setAlerted;
    bool m_open = false;
    bool m_recording = true;
    bool m_broken = false; // a write in the transaction under way failed
    bool m_current = false; // the locked snapshot includes every outside write
    bool m_externalChange = false;
    bool m_undoChanged = false;
    int m_depth = 0;
    qint64 m_dataVersion = 0;

    QVector<TaskList> m_lists;
    QVector<Section> m_sections;
    QHash<qint64, Reminder> m_reminders;

    UndoEntry m_pending;
    QVector<UndoEntry> m_undo, m_redo;

    QFileSystemWatcher m_watcher;
    QTimer m_reload;
    bool m_watching = false;
    std::function<void()> m_loadHook;
};
