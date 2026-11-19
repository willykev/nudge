#pragma once

#include "store.h"

#include <QAbstractListModel>
#include <QSet>

// Which reminders the window is showing.
struct View {
    enum Kind { Today, Scheduled, All, Flagged, Completed, List, Tag, Search };

    Kind kind = Today;
    qint64 listId = 0;
    QString text; // the tag, or the words searched for

    QString key() const;
    static View fromKey(const QString &key);
    // Whether rows are a list's own order with subtasks under their parents,
    // rather than reminders gathered from everywhere.
    bool isTree() const { return kind == List || kind == All; }
    bool operator==(const View &) const = default;
};

struct Row {
    qint64 key = 0;       // the same across refreshes: the reminder's id, negative for a heading
    qint64 id = 0;        // the reminder, 0 for a heading
    bool heading = false;
    QString text;         // what a heading says
    QString color;        // and in which color, empty for the default
    qint64 sectionId = 0; // set on the heading of a list's own section
    int depth = 0;
    int children = 0;
    bool collapsed = false;
};

struct ViewOptions {
    QDateTime now;
    bool showCompleted = false;
    QSet<qint64> lingering; // completed a moment ago and still on screen
    QSet<qint64> collapsed; // parents whose subtasks are folded away
    qint64 draftId = 0; // only this window's inline editor shows its draft
};

QVector<Row> buildRows(const Store &store, const View &view, const ViewOptions &options);

// The rows of the current view. A refresh is applied as the smallest set of
// insertions, removals, and moves, so the list on screen keeps its place and
// animates instead of being rebuilt.
class ReminderModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        RidRole,
        TitleRole,
        NotesRole,
        DoneRole,
        FlaggedRole,
        PriorityRole,
        DueTextRole,
        OverdueRole,
        RepeatTextRole,
        TagsRole,
        LinkRole,
        DepthRole,
        ChildCountRole,
        CollapsedRole,
        ListNameRole,
        TintRole,
        ParentTitleRole,
        HeadingRole,
        SectionIdRole,
    };

    explicit ReminderModel(Store *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void show(const QVector<Row> &rows, bool reset);
    void setContext(const QDateTime &now, bool namesLists);
    void touch(); // the clock moved on: dates read differently
    const QVector<Row> &rows() const { return m_rows; }
    int rowOf(qint64 reminderId) const;
    qint64 idAt(int row) const;

    static QString describeRepeat(const QString &rule);
    static QString linkLabel(const QString &url);
    static bool isOverdue(const Reminder &reminder, const QDateTime &now);

private:
    void reindex();

    Store *m_store;
    QVector<Row> m_rows;
    QHash<qint64, int> m_rowOf;
    QDateTime m_now;
    bool m_namesLists = false;
};
