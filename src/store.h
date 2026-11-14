#pragma once

#include <QDate>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTimer>
#include <QVector>
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
    QString repeat;      // see recurrence.h
    QDate repeatEnd;
    int early = 0;       // minutes before due for an extra alert, 0 for none
    int priority = 0;    // 0 none, 1 low, 2 medium, 3 high
    bool flagged = false;
    QStringList tags;
    bool completed = false;
    qint64 completedAt = 0; // seconds since the epoch, like the two below
    qint64 createdAt = 0;
    qint64 modifiedAt = 0;
    double position = 0;
    // Alert bookkeeping: the moments already announced, and a snooze.
    qint64 alertedDue = 0, alertedEarly = 0, snoozeUntil = 0;

    bool operator==(const Reminder &) const = default;
    bool sameContent(const Reminder &other) const; // ignores the bookkeeping
    qint64 dueAt() const; // when the alert is due, 0 without a date
};

// Every list, section, and reminder, held in memory and written through to
// SQLite as it changes. Changes are grouped into transactions that undo and
// redo as one step, and other processes (the command line, the background
// alerts) share the same file.
class Store : public QObject {
    Q_OBJECT

public:
    // Groups the changes made while it is alive into one undo step.
    class Tx {
    public:
        explicit Tx(Store *store, const QString &mergeKey = {}) : m_store(store) { store->begin(mergeKey); }
        ~Tx() { m_store->commit(); }
        Tx(const Tx &) = delete;
        Tx &operator=(const Tx &) = delete;

    private:
        Store *m_store;
    };

    explicit Store(const QString &path = {}, QObject *parent = nullptr);
    ~Store() override;

    static QString defaultPath();
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

    void begin(const QString &mergeKey = {});
    void commit();
    qint64 put(Reminder reminder);
    qint64 put(TaskList list);
    qint64 put(Section section);
    void removeReminder(qint64 id); // and its subtasks
    void removeReminders(const QVector<qint64> &ids); // exactly these
    void removeList(qint64 id);     // and everything in it
    void removeSection(qint64 id);  // its reminders stay in the list

    // Completing a repeating reminder leaves a completed copy behind and
    // moves the reminder to its next date, which is returned.
    QDate complete(qint64 id, const QDateTime &now);
    void reopen(qint64 id);

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

signals:
    void changed();
    void undoChanged();
    void failed(const QString &message);

private:
    template <typename T> struct Delta {
        qint64 id = 0;
        std::optional<T> before, after;
    };
    struct UndoEntry {
        QString mergeKey;
        QVector<Delta<TaskList>> lists;
        QVector<Delta<Section>> sections;
        QVector<Delta<Reminder>> reminders;
        bool isEmpty() const { return lists.isEmpty() && sections.isEmpty() && reminders.isEmpty(); }
    };

    bool run(const QString &sql);
    void fail(const QString &message);
    void prepare();
    void load();
    qint64 dataVersion();
    void arm();
    bool write(Reminder &reminder);
    bool write(TaskList &list);
    bool write(Section &section);
    void erase(const char *table, qint64 id);
    void eraseReminder(qint64 id);
    void apply(qint64 id, const std::optional<Reminder> &state);
    void apply(qint64 id, const std::optional<TaskList> &state);
    void apply(qint64 id, const std::optional<Section> &state);
    void replay(const UndoEntry &entry, bool forward);
    void sortLists();

    QString m_path, m_connection, m_error;
    QSqlDatabase m_db;
    QSqlQuery m_putReminder, m_putList, m_putSection, m_setAlerted;
    bool m_open = false;
    bool m_recording = true;
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
};
