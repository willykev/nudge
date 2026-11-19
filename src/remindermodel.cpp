#include "remindermodel.h"

#include "dates.h"
#include "recurrence.h"

#include <QCoreApplication>
#include <QUrl>
#include <algorithm>
#include <functional>

namespace {

// Heading keys are negative, in ranges that cannot meet.
constexpr qint64 SectionKeys = 1'000'000'000'000LL;
constexpr qint64 ListKeys = 2'000'000'000'000LL;
constexpr qint64 DayKeys = 3'000'000'000'000LL;
constexpr qint64 MonthKeys = 4'000'000'000'000LL;
constexpr qint64 GroupKeys = 5'000'000'000'000LL;
constexpr qint64 DoneMonthKeys = 6'000'000'000'000LL;
enum Group { Overdue = 1, DueToday, DoneToday, DoneYesterday, DoneWeek, DoneMonth };

using Items = QVector<const Reminder *>;
using Order = std::function<bool(const Reminder *, const Reminder *)>;

QString translate(const char *text) { return QCoreApplication::translate("Views", text); }

bool byPosition(const Reminder *a, const Reminder *b) {
    return a->position != b->position ? a->position < b->position : a->id < b->id;
}

bool byDue(const Reminder *a, const Reminder *b) {
    if (a->due.isValid() != b->due.isValid())
        return a->due.isValid();
    if (a->due != b->due)
        return a->due < b->due;
    if (a->dueMinutes != b->dueMinutes)
        return a->dueMinutes < b->dueMinutes;
    return byPosition(a, b);
}

Order orderFor(const QString &sort) {
    if (sort == "due")
        return byDue;
    if (sort == "priority")
        return [](const Reminder *a, const Reminder *b) {
            return a->priority != b->priority ? a->priority > b->priority : byPosition(a, b);
        };
    if (sort == "title")
        return [](const Reminder *a, const Reminder *b) {
            const int order = a->title.compare(b->title, Qt::CaseInsensitive);
            return order != 0 ? order < 0 : byPosition(a, b);
        };
    if (sort == "created")
        return [](const Reminder *a, const Reminder *b) {
            return a->createdAt != b->createdAt ? a->createdAt < b->createdAt : a->id < b->id;
        };
    return byPosition;
}

Row heading(qint64 key, const QString &text, const QString &color = {}, qint64 sectionId = 0) {
    Row row;
    row.key = -key;
    row.heading = true;
    row.text = text;
    row.color = color;
    row.sectionId = sectionId;
    return row;
}

Row item(const Reminder *reminder, int depth = 0) {
    Row row;
    row.key = row.id = reminder->id;
    row.depth = depth;
    return row;
}

bool matches(const Reminder &r, const QStringList &terms) {
    for (const QString &term : terms) {
        const bool tagOnly = term.size() > 1 && term.startsWith('#');
        const QString word = tagOnly ? term.mid(1) : term;
        bool found = !tagOnly && (r.title.contains(word, Qt::CaseInsensitive) ||
                                 r.notes.contains(word, Qt::CaseInsensitive) || r.url.contains(word, Qt::CaseInsensitive));
        for (int i = 0; !found && i < r.tags.size(); ++i)
            found = r.tags[i].contains(word, Qt::CaseInsensitive);
        if (!found)
            return false;
    }
    return true;
}

} // namespace

QString View::key() const {
    switch (kind) {
    case Today: return "today";
    case Scheduled: return "scheduled";
    case All: return "all";
    case Flagged: return "flagged";
    case Completed: return "completed";
    case List: return "list:" + QString::number(listId);
    case Tag: return "tag:" + text;
    case Search: return "search";
    }
    return "today";
}

View View::fromKey(const QString &key) {
    View view;
    if (key == "scheduled")
        view.kind = Scheduled;
    else if (key == "all")
        view.kind = All;
    else if (key == "flagged")
        view.kind = Flagged;
    else if (key == "completed")
        view.kind = Completed;
    else if (key.startsWith("list:")) {
        view.kind = List;
        view.listId = key.mid(5).toLongLong();
    } else if (key.startsWith("tag:")) {
        view.kind = Tag;
        view.text = key.mid(4);
    }
    return view;
}

QVector<Row> buildRows(const Store &store, const View &view, const ViewOptions &options) {
    QVector<Row> rows;
    const QDate today = options.now.date();
    auto shown = [&](const Reminder &r) {
        return !r.completed || options.showCompleted || options.lingering.contains(r.id);
    };
    auto belongs = [&](const Reminder &r) {
        return r.id == options.draftId || (!r.draft && r.hasContent());
    };

    if (view.isTree()) {
        // One pass sorts every reminder under its list or its parent.
        QHash<qint64, Items> top, children;
        for (const Reminder &r : store.reminders()) {
            if (!belongs(r))
                continue;
            if (view.kind == View::List && r.listId != view.listId)
                continue;
            const Reminder *parent = r.parentId ? store.reminder(r.parentId) : nullptr;
            if (parent && parent->listId == r.listId && belongs(*parent))
                children[r.parentId].append(&r);
            else
                top[r.listId].append(&r);
        }
        for (const TaskList &list : store.lists()) {
            if (view.kind == View::List && list.id != view.listId)
                continue;
            const Order order = orderFor(list.sort);
            QVector<qint64> sectionIds{0};
            for (const Section &section : store.sections())
                if (section.listId == list.id)
                    sectionIds.append(section.id);
            QHash<qint64, Items> bySection;
            for (const Reminder *r : top.value(list.id))
                bySection[sectionIds.contains(r->sectionId) ? r->sectionId : 0].append(r);

            QVector<Row> listRows;
            for (qint64 sectionId : sectionIds) {
                // A list shows its sections even when empty, so there is
                // somewhere to put things; All only shows what there is.
                if (sectionId && view.kind == View::List)
                    listRows.append(heading(SectionKeys + sectionId, store.section(sectionId)->name, {}, sectionId));
                Items items = bySection.value(sectionId);
                std::sort(items.begin(), items.end(), order);
                for (const Reminder *r : items) {
                    Items subtasks = children.value(r->id);
                    std::sort(subtasks.begin(), subtasks.end(), order);
                    if (!shown(*r)) {
                        // Out of sight goes what it holds, except a subtask still
                        // open: that shows on its own, in its place, so nothing
                        // left to do drops out of the list.
                        for (const Reminder *subtask : subtasks)
                            if (shown(*subtask))
                                listRows.append(item(subtask));
                        continue;
                    }
                    Row row = item(r);
                    row.children = subtasks.size();
                    row.collapsed = options.collapsed.contains(r->id);
                    listRows.append(row);
                    if (row.collapsed)
                        continue;
                    for (const Reminder *subtask : subtasks)
                        if (shown(*subtask))
                            listRows.append(item(subtask, 1));
                }
            }
            if (view.kind == View::All) {
                if (listRows.isEmpty())
                    continue;
                rows.append(heading(ListKeys + list.id, list.name, list.color));
            }
            rows += listRows;
        }
        return rows;
    }

    Items items;
    const QStringList terms = view.text.split(' ', Qt::SkipEmptyParts);
    for (const Reminder &r : store.reminders()) {
        if (!belongs(r))
            continue;
        const bool lingering = options.lingering.contains(r.id);
        bool include = false;
        switch (view.kind) {
        case View::Today:
            // Finished reminders from days ago are history, not today.
            include = r.due.isValid() && r.due <= today &&
                      (!r.completed || lingering || (options.showCompleted && r.due == today));
            break;
        case View::Scheduled:
            include = r.due.isValid() && (!r.completed || lingering || (options.showCompleted && r.due >= today));
            break;
        case View::Flagged:
            include = r.flagged && shown(r);
            break;
        case View::Completed:
            include = r.completed;
            break;
        case View::Tag:
            include = shown(r) && r.tags.contains(view.text, Qt::CaseInsensitive);
            break;
        case View::Search:
            include = !terms.isEmpty() && matches(r, terms);
            break;
        default:
            break;
        }
        if (include)
            items.append(&r);
    }

    if (view.kind == View::Completed) {
        std::sort(items.begin(), items.end(), [](const Reminder *a, const Reminder *b) {
            return a->completedAt != b->completedAt ? a->completedAt > b->completedAt : a->id > b->id;
        });
        qint64 last = 0;
        for (const Reminder *r : items) {
            const QDate day = QDateTime::fromSecsSinceEpoch(r->completedAt).date();
            const qint64 ago = day.daysTo(today);
            qint64 key;
            QString text;
            if (ago <= 0) {
                key = GroupKeys + DoneToday;
                text = translate(QT_TRANSLATE_NOOP("Views", "Today"));
            } else if (ago == 1) {
                key = GroupKeys + DoneYesterday;
                text = translate(QT_TRANSLATE_NOOP("Views", "Yesterday"));
            } else if (ago <= 7) {
                key = GroupKeys + DoneWeek;
                text = translate(QT_TRANSLATE_NOOP("Views", "Previous 7 Days"));
            } else if (ago <= 30) {
                key = GroupKeys + DoneMonth;
                text = translate(QT_TRANSLATE_NOOP("Views", "Previous 30 Days"));
            } else {
                key = DoneMonthKeys + day.year() * 12 + day.month();
                text = Dates::formatMonth(day);
            }
            if (key != last)
                rows.append(heading(key, text));
            last = key;
            rows.append(item(r));
        }
        return rows;
    }

    if (view.kind == View::Search) {
        std::sort(items.begin(), items.end(), [](const Reminder *a, const Reminder *b) {
            if (a->completed != b->completed)
                return b->completed;
            return a->modifiedAt != b->modifiedAt ? a->modifiedAt > b->modifiedAt : a->id > b->id;
        });
    } else {
        std::sort(items.begin(), items.end(), byDue);
    }

    if (view.kind == View::Today) {
        // Headings only earn their place when there is something overdue to
        // set apart.
        const bool anyOverdue = !items.isEmpty() && items.first()->due < today;
        qint64 last = 0;
        for (const Reminder *r : items) {
            const qint64 key = GroupKeys + (r->due < today ? Overdue : DueToday);
            if (anyOverdue && key != last)
                rows.append(r->due < today
                                ? heading(key, translate(QT_TRANSLATE_NOOP("Views", "Overdue")), "red")
                                : heading(key, translate(QT_TRANSLATE_NOOP("Views", "Due Today"))));
            last = key;
            rows.append(item(r));
        }
        return rows;
    }

    if (view.kind == View::Scheduled) {
        qint64 last = 0;
        for (const Reminder *r : items) {
            const qint64 ahead = today.daysTo(r->due);
            qint64 key;
            if (ahead < 0)
                key = GroupKeys + Overdue;
            else if (ahead < 7)
                key = DayKeys + r->due.toJulianDay();
            else
                key = MonthKeys + r->due.year() * 12 + r->due.month();
            if (key != last)
                rows.append(ahead < 0   ? heading(key, translate(QT_TRANSLATE_NOOP("Views", "Overdue")), "red")
                            : ahead < 7 ? heading(key, Dates::formatDay(r->due, today))
                                        : heading(key, Dates::formatMonth(r->due)));
            last = key;
            rows.append(item(r));
        }
        return rows;
    }

    for (const Reminder *r : items)
        rows.append(item(r));
    return rows;
}

ReminderModel::ReminderModel(Store *store, QObject *parent)
    : QAbstractListModel(parent), m_store(store), m_now(QDateTime::currentDateTime()) {}

int ReminderModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> ReminderModel::roleNames() const {
    return {{KindRole, "kind"},
            {RidRole, "rid"},
            {TitleRole, "title"},
            {NotesRole, "notes"},
            {DoneRole, "done"},
            {FlaggedRole, "flagged"},
            {PriorityRole, "priority"},
            {DueTextRole, "dueText"},
            {OverdueRole, "overdue"},
            {RepeatTextRole, "repeatText"},
            {TagsRole, "tags"},
            {LinkRole, "link"},
            {DepthRole, "depth"},
            {ChildCountRole, "childCount"},
            {CollapsedRole, "collapsed"},
            {ListNameRole, "listName"},
            {TintRole, "tint"},
            {ParentTitleRole, "parentTitle"},
            {HeadingRole, "heading"},
            {SectionIdRole, "sectionId"}};
}

QVariant ReminderModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows[index.row()];
    const Reminder *r = row.heading ? nullptr : m_store->reminder(row.id);
    const TaskList *list = r ? m_store->list(r->listId) : nullptr;

    // Every role answers with its own type, also on a heading, so a delegate
    // never sees undefined.
    switch (role) {
    case KindRole: return row.heading ? QStringLiteral("heading") : QStringLiteral("reminder");
    case RidRole: return row.id;
    case HeadingRole: return row.text;
    case SectionIdRole: return row.sectionId;
    case DepthRole: return row.depth;
    case ChildCountRole: return row.children;
    case CollapsedRole: return row.collapsed;
    case TintRole: return row.heading ? row.color : list ? list->color : QString();
    case TitleRole: return r ? r->title : QString();
    case NotesRole: return r ? r->notes : QString();
    case DoneRole: return r && r->completed;
    case FlaggedRole: return r && r->flagged;
    case PriorityRole: return r ? r->priority : 0;
    case DueTextRole: return r ? Dates::formatDue(r->due, r->dueMinutes, m_now.date()) : QString();
    case OverdueRole: return r && isOverdue(*r, m_now);
    case RepeatTextRole: return r ? describeRepeat(r->repeat) : QString();
    case TagsRole: return r && !r->tags.isEmpty() ? "#" + r->tags.join(QStringLiteral("  #")) : QString();
    case LinkRole: return r ? linkLabel(r->url) : QString();
    case ListNameRole: return r && list && m_namesLists ? list->name : QString();
    case ParentTitleRole: {
        // Away from its parent, a subtask says whose it is: in a view gathered
        // from everywhere, or on its own in a list while its parent is hidden.
        const Reminder *parent =
            r && r->parentId && (m_namesLists || row.depth == 0) ? m_store->reminder(r->parentId) : nullptr;
        return parent ? parent->title : QString();
    }
    }
    return {};
}

void ReminderModel::setContext(const QDateTime &now, bool namesLists) {
    m_now = now;
    m_namesLists = namesLists;
}

void ReminderModel::touch() {
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {DueTextRole, OverdueRole});
}

void ReminderModel::reindex() {
    m_rowOf.clear();
    m_rowOf.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i)
        if (!m_rows[i].heading)
            m_rowOf.insert(m_rows[i].id, i);
}

int ReminderModel::rowOf(qint64 reminderId) const {
    return m_rowOf.value(reminderId, -1);
}

qint64 ReminderModel::idAt(int row) const {
    return row >= 0 && row < m_rows.size() ? m_rows[row].id : 0;
}

void ReminderModel::show(const QVector<Row> &next, bool reset) {
    auto replace = [&] {
        beginResetModel();
        m_rows = next;
        reindex();
        endResetModel();
    };
    QSet<qint64> nextKeys;
    nextKeys.reserve(next.size());
    for (const Row &row : next)
        nextKeys.insert(row.key);
    if (reset || next.isEmpty() || nextKeys.size() != next.size()) {
        replace();
        return;
    }
    qsizetype shared = 0;
    for (const Row &row : std::as_const(m_rows))
        shared += nextKeys.contains(row.key);
    // Small changes keep their animations. A large set of interleaved
    // insertions or removals would shift the row vector over and over.
    if (next.size() + m_rows.size() - 2 * shared > 128) {
        replace();
        return;
    }

    // Rows that are gone, from the bottom up so earlier indexes hold.
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (nextKeys.contains(m_rows[i].key))
            continue;
        int first = i;
        while (first > 0 && !nextKeys.contains(m_rows[first - 1].key))
            --first;
        beginRemoveRows({}, first, i);
        m_rows.remove(first, i - first + 1);
        endRemoveRows();
        i = first;
    }

    // Rows that stay, moved into their new order. A reminder changing place
    // is one move; a whole new order is cheaper to show from scratch.
    QSet<qint64> kept;
    kept.reserve(m_rows.size());
    for (const Row &row : m_rows)
        kept.insert(row.key);
    QVector<qint64> order;
    order.reserve(m_rows.size());
    for (const Row &row : next)
        if (kept.contains(row.key))
            order.append(row.key);
    int moves = 0;
    for (int i = 0; i < order.size(); ++i) {
        if (m_rows[i].key == order[i])
            continue;
        if (++moves > 32) {
            replace();
            return;
        }
        int from = i + 1;
        while (m_rows[from].key != order[i])
            ++from;
        beginMoveRows({}, from, from, {}, i);
        m_rows.move(from, i);
        endMoveRows();
    }

    // Rows that are new.
    for (int i = 0; i < next.size(); ++i) {
        if (i < m_rows.size() && m_rows[i].key == next[i].key)
            continue;
        int last = i;
        while (last + 1 < next.size() && !kept.contains(next[last + 1].key))
            ++last;
        beginInsertRows({}, i, last);
        for (int k = i; k <= last; ++k)
            m_rows.insert(k, next[k]);
        endInsertRows();
        i = last;
    }

    // And what every row now says. Only the rows on screen are asked again.
    m_rows = next;
    reindex();
    emit dataChanged(index(0), index(m_rows.size() - 1));
}

bool ReminderModel::isOverdue(const Reminder &r, const QDateTime &now) {
    if (r.completed || !r.due.isValid())
        return false;
    if (r.dueMinutes < 0)
        return r.due < now.date();
    return r.dueAt(now.timeRepresentation()) < now.toSecsSinceEpoch() - now.time().second();
}

QString ReminderModel::describeRepeat(const QString &rule) {
    const Repeat repeat = Repeat::parse(rule);
    const int n = repeat.interval;
    switch (repeat.unit) {
    case Repeat::Never: break;
    case Repeat::Weekdays: return tr("Weekdays");
    case Repeat::Daily: return n == 1 ? tr("Daily") : tr("Every %n days", "", n);
    case Repeat::Weekly: return n == 1 ? tr("Weekly") : tr("Every %n weeks", "", n);
    case Repeat::Monthly: return n == 1 ? tr("Monthly") : tr("Every %n months", "", n);
    case Repeat::Yearly: return n == 1 ? tr("Yearly") : tr("Every %n years", "", n);
    }
    return {};
}

// A link shows as the site it leads to. Any other kind says what kind it is
// too (claude://claude.ai), so it never passes for a web page.
QString ReminderModel::linkLabel(const QString &url) {
    const QString text = url.trimmed();
    if (text.isEmpty())
        return {};
    const QUrl link = QUrl::fromUserInput(text);
    QString host = link.host();
    if (host.isEmpty())
        return text;
    if (link.scheme() != "http" && link.scheme() != "https")
        return link.scheme() + "://" + host;
    if (host.startsWith("www."))
        host.remove(0, 4);
    return host;
}
