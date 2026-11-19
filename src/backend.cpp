#include "backend.h"

#include "dates.h"
#include "recurrence.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <QColor>
#include <QDBusConnection>
#include <QDesktopServices>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>

namespace {
const QStringList repeatUnits{"never", "daily", "weekdays", "weekly", "monthly", "yearly"};
const QStringList sortModes{"manual", "due", "priority", "title", "created"};

// A monthly or yearly reminder keeps returning to the day it is set for.
void anchor(Reminder &r) {
    Repeat repeat = Repeat::parse(r.repeat);
    if (repeat.unit != Repeat::Monthly && repeat.unit != Repeat::Yearly)
        return;
    repeat.anchorDay = r.due.day();
    r.repeat = repeat.toString();
}
} // namespace

Backend::Backend(Store *store, QObject *parent)
    : QObject(parent), m_store(store), m_model(new ReminderModel(store, this)), m_sidebar(new SidebarModel(this)),
      m_clock([] { return QDateTime::currentDateTime(); }) {
    m_showCompleted = m_store->meta("show_completed") == "1";
    m_backgroundAsked = m_store->meta("background_asked") == "1";
    m_day = now().date();

    // A completed reminder stays where it is for a moment, so a slip of the
    // finger can be taken back before the row is gone.
    m_linger.setSingleShot(true);
    m_linger.setInterval(1600);
    connect(&m_linger, &QTimer::timeout, this, [this] {
        m_lingering.clear();
        refresh();
    });

    // Midnight changes what Today holds; any minute can make something overdue.
    m_tick.setInterval(20000);
    connect(&m_tick, &QTimer::timeout, this, &Backend::tick);
    m_tick.start();
    // The timer stands still while the machine sleeps; a laptop opened the
    // next morning should not show yesterday as Today.
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
                                         "org.freedesktop.login1.Manager", "PrepareForSleep", this,
                                         SLOT(onPrepareForSleep(bool)));

    connect(m_store, &Store::changed, this, [this] { refresh(); });
    connect(m_store, &Store::undoChanged, this, &Backend::undoChanged);
    connect(m_store, &Store::failed, this, &Backend::problemChanged);
    // A change the file refused is said once the action is over, so the word
    // the action has for success does not cover it up.
    connect(
        m_store, &Store::failed, this,
        [this](const QString &message) { emit notice(tr("Nudge could not save a change: %1").arg(message)); },
        Qt::QueuedConnection);

    // A reminder left without a title is one the app never got to finish
    // (it was killed mid-word), not something to be reminded of. One touched
    // in the last few minutes may be another window's, still being typed.
    m_store->cleanUpDrafts(now().toSecsSinceEpoch() - 10 * 60);

    m_view = View::fromKey(m_store->meta("view"));
    refresh(true);
    selectNear(0);
    QTimer::singleShot(0, this, &Backend::checkBackground);
}

void Backend::tick() {
    const QDate today = now().date();
    if (today != m_day) {
        m_day = today;
        refresh();
    } else {
        m_model->setContext(now(), !m_view.isTree());
        m_model->touch();
    }
    emit problemChanged();
}

void Backend::onPrepareForSleep(bool sleeping) {
    if (!sleeping)
        tick();
}

Backend::~Backend() {
    // The window opens next time where it was left.
    if (m_view.kind != View::Search)
        m_store->setMeta("view", m_view.key());
}

void Backend::setClock(std::function<QDateTime()> clock) {
    m_clock = std::move(clock);
    m_day = now().date();
    refresh(true);
}

void Backend::setService(const QString &systemctl, const QString &unitFile) {
    m_systemctl = systemctl;
    m_unitFile = unitFile;
    checkBackground();
}

void Backend::attachAlerts(Alerts *alerts) {
    m_alerts = alerts;
    m_notifier = new Notifier(m_store, alerts, this);
    connect(m_notifier, &Notifier::opened, this, &Backend::reveal);
    if (auto *desktop = dynamic_cast<DesktopAlerts *>(alerts))
        connect(desktop, &DesktopAlerts::acted, m_notifier,
                [this](quint32 id, const QString &action) { m_notifier->act(id, action, now()); });
    m_notifier->start();
    emit problemChanged();
}

void Backend::retranslate() {
    refresh(true);
    emit viewChanged();
}

QString Backend::problem() const {
    if (!m_store->isOpen())
        return tr("Nudge cannot save reminders: %1").arg(m_store->error());
    return m_alerts ? m_alerts->problem() : QString();
}

// ---- The view --------------------------------------------------------------

void Backend::refresh(bool reset) {
    bool viewMoved = false;
    if (m_view.kind == View::List && !m_store->list(m_view.listId)) {
        // The list is gone: deleted here, undone, or removed from the command line.
        m_view = View();
        reset = viewMoved = true;
    }

    ViewOptions options;
    options.now = now();
    options.showCompleted = m_showCompleted;
    options.lingering = m_lingering;
    options.collapsed = m_collapsed;
    options.draftId = m_draftId;

    const int row = m_model->rowOf(m_selectedId);
    m_model->setContext(options.now, !m_view.isTree());
    m_model->show(buildRows(*m_store, m_view, options), reset);

    if (m_draftId && !m_store->reminder(m_draftId)) {
        m_draftId = 0;
        m_draftHeld = false;
        emit draftChanged();
    }
    // Something stays selected: when the selected reminder leaves the view,
    // the one that took its place is next. A new reminder the details pane is
    // filling in stays, though what the pane gave it took it out of the view.
    if (m_model->rowOf(m_selectedId) < 0 && !(m_draftHeld && m_selectedId == m_draftId))
        selectNear(qMax(0, row));

    updateSidebar();
    // A tag's view is written the way its entry is, so the two stay one.
    if (m_view.kind == View::Tag)
        for (const SidebarEntry &entry : m_sidebar->entries())
            if (entry.group == "tags" && entry.name.toLower() == m_view.text.toLower())
                m_view.text = entry.name;

    int open = 0;
    for (const Row &shown : m_model->rows()) {
        const Reminder *r = shown.heading ? nullptr : m_store->reminder(shown.id);
        open += r && !r->completed && !r->draft;
    }
    if (open != m_openCount) {
        m_openCount = open;
        emit countsChanged();
    }
    if (m_store->lists() != m_listsSeen || m_store->sections() != m_sectionsSeen) {
        m_listsSeen = m_store->lists();
        m_sectionsSeen = m_store->sections();
        emit listsChanged();
    }
    // The sections on offer and the sort are the list on screen's, so they
    // change with the view and with an undo as much as with the store.
    const QVariantList sections = this->sections();
    if (sections != m_sections) {
        m_sections = sections;
        emit sectionsChanged();
    }
    const QString title = viewTitle(), color = viewColor(), sort = sortMode();
    if (viewMoved || title != m_title || color != m_color || sort != m_sort) {
        m_title = title;
        m_color = color;
        m_sort = sort;
        emit viewChanged();
    }
    rememberSelection();
}

void Backend::rememberSelection() {
    const int row = selectedRow();
    if (row != m_selectedRow) {
        m_selectedRow = row;
        emit selectedRowChanged();
    }
    emit selectedChanged();
}

void Backend::updateSidebar() {
    const QDate today = now().date();
    int dueToday = 0, scheduled = 0, open = 0, flagged = 0;
    QHash<qint64, int> perList;
    QMap<QString, QMap<QString, int>> tags; // by lowercase name: each way it is written, and how often
    for (const Reminder &r : m_store->reminders()) {
        if (r.completed || r.draft || !r.hasContent())
            continue;
        ++open;
        ++perList[r.listId];
        if (r.due.isValid()) {
            ++scheduled;
            dueToday += r.due <= today;
        }
        flagged += r.flagged;
        for (const QString &tag : r.tags)
            ++tags[tag.toLower()][tag];
    }

    QVector<SidebarEntry> entries{
        {"today", "", tr("Today"), "", "today", dueToday},
        {"scheduled", "", tr("Scheduled"), "red", "calendar", scheduled},
        {"all", "", tr("All"), "dim", "tray", open},
        {"flagged", "", tr("Flagged"), "orange", "flag", flagged},
        {"completed", "", tr("Completed"), "green", "check", -1},
    };
    for (const TaskList &list : m_store->lists())
        entries.append({"list:" + QString::number(list.id), "lists", list.name, list.color, "list",
                        perList.value(list.id)});
    for (const QMap<QString, int> &spellings : std::as_const(tags)) {
        // A tag goes by the way most reminders write it, and on a tie by the
        // one that sorts first, so its entry is the same from launch to launch.
        QString name;
        int most = 0, count = 0;
        for (auto it = spellings.cbegin(); it != spellings.cend(); ++it) {
            count += it.value();
            if (it.value() > most) {
                most = it.value();
                name = it.key();
            }
        }
        entries.append({"tag:" + name, "tags", name, "", "tag", count});
    }
    m_sidebar->show(entries);
}

void Backend::setViewKey(const QString &key) {
    const View view = View::fromKey(key);
    if (view.kind == View::List && !m_store->list(view.listId))
        return;
    if (!m_search.isEmpty()) {
        m_search.clear();
        emit searchChanged();
    }
    if (view == m_view)
        return;
    m_view = view;
    m_lingering.clear();
    m_selectedId = 0;
    refresh(true);
    selectNear(0);
    emit selectedIdChanged();
    emit viewChanged();
    // The view leaves a new reminder the details pane was filling in, once
    // the pane has saved what was typed for it.
    settleDraft();
}

QString Backend::viewKind() const {
    static const QStringList kinds{"today", "scheduled", "all", "flagged", "completed", "list", "tag", "search"};
    return kinds.value(m_view.kind);
}

QString Backend::viewTitle() const {
    switch (m_view.kind) {
    case View::Today: return tr("Today");
    case View::Scheduled: return tr("Scheduled");
    case View::All: return tr("All");
    case View::Flagged: return tr("Flagged");
    case View::Completed: return tr("Completed");
    case View::Tag: return "#" + m_view.text;
    case View::Search: return tr("Results for “%1”").arg(m_view.text);
    case View::List:
        if (const TaskList *list = m_store->list(m_view.listId))
            return list->name;
    }
    return {};
}

QString Backend::viewColor() const {
    switch (m_view.kind) {
    // Names of the theme's colors, which the window looks up: a view keeps
    // its meaning, and its exact shade follows the theme.
    case View::Scheduled: return "red";
    case View::All: return "dim";
    case View::Flagged: return "orange";
    case View::Completed: return "green";
    case View::List:
        if (const TaskList *list = m_store->list(m_view.listId))
            return list->color;
        break;
    default: break;
    }
    return {}; // the theme accent
}

QString Backend::sortMode() const {
    const TaskList *list = m_view.kind == View::List ? m_store->list(m_view.listId) : nullptr;
    return list ? list->sort : QStringLiteral("manual");
}

void Backend::setSortMode(const QString &mode) {
    const TaskList *existing = m_view.kind == View::List ? m_store->list(m_view.listId) : nullptr;
    if (!existing || !sortModes.contains(mode) || existing->sort == mode)
        return;
    TaskList list = *existing;
    list.sort = mode;
    m_store->put(list);
    emit viewChanged();
}

QString Backend::sortName(const QString &mode) const {
    return mode == "due"        ? tr("Due Date")
           : mode == "priority" ? tr("Priority")
           : mode == "title"    ? tr("Title")
           : mode == "created"  ? tr("Date Created")
                                : tr("Manual Order");
}

void Backend::cycleSort() {
    if (m_view.kind != View::List) {
        emit notice(tr("Open a list to change how it is sorted"));
        return;
    }
    const QString next = sortModes[(sortModes.indexOf(sortMode()) + 1) % sortModes.size()];
    setSortMode(next);
    emit notice(tr("Sorted by %1").arg(sortName(next)));
}

void Backend::setShowCompleted(bool show) {
    if (show == m_showCompleted)
        return;
    m_showCompleted = show;
    m_store->setMeta("show_completed", show ? "1" : "0");
    emit showCompletedChanged();
    refresh();
}

void Backend::setSearch(const QString &text) {
    if (text == m_search)
        return;
    m_search = text;
    emit searchChanged();
    const QString query = text.simplified();
    if (!query.isEmpty()) {
        if (m_view.kind != View::Search)
            m_viewBeforeSearch = m_view;
        m_view = View{View::Search, 0, query};
    } else if (m_view.kind == View::Search) {
        m_view = m_viewBeforeSearch;
    } else {
        return;
    }
    m_lingering.clear();
    m_selectedId = 0;
    refresh(true);
    selectNear(0);
    emit selectedIdChanged();
    emit viewChanged();
    settleDraft(); // as a view does
}

// ---- Selection ---------------------------------------------------------------

void Backend::setSelectedId(qint64 id) {
    // One gone meanwhile, as a new reminder its own box just discarded, is
    // no place for the selection.
    if (id == m_selectedId || (id && !m_store->reminder(id)))
        return;
    m_selectedId = id;
    // Edits to another reminder are another step to undo. A reminder being
    // added is still the same step until its title is in, wherever the
    // selection went in the meantime: naming it seals the step.
    if (!m_draftId)
        m_store->sealUndo();
    emit selectedIdChanged();
    rememberSelection();
    // Left by the selection, one the details pane was filling in is settled,
    // once the pane has saved what was typed for it.
    if (m_draftHeld && id != m_draftId)
        settleDraft();
}

void Backend::selectNear(int row) {
    const QVector<Row> &rows = m_model->rows();
    qint64 id = 0;
    for (int i = qMax(0, row); i < rows.size() && !id; ++i)
        if (!rows[i].heading)
            id = rows[i].id;
    for (int i = qMin(row, int(rows.size()) - 1); i >= 0 && !id; --i)
        if (!rows[i].heading)
            id = rows[i].id;
    if (id == m_selectedId) {
        rememberSelection();
        return;
    }
    setSelectedId(id);
}

void Backend::selectStep(int delta) {
    const QVector<Row> &rows = m_model->rows();
    int row = selectedRow();
    if (row < 0) {
        selectEdge(delta < 0);
        return;
    }
    const int step = delta < 0 ? -1 : 1;
    for (int left = std::abs(delta); left > 0; --left) {
        int next = row + step;
        while (next >= 0 && next < rows.size() && rows[next].heading)
            next += step;
        if (next < 0 || next >= rows.size())
            break;
        row = next;
    }
    setSelectedId(rows[row].id);
}

void Backend::selectEdge(bool last) {
    selectNear(last ? m_model->rows().size() - 1 : 0);
}

void Backend::stepView(int delta) {
    const QVector<SidebarEntry> &entries = m_sidebar->entries();
    if (entries.isEmpty())
        return;
    const int current = m_sidebar->indexOf(m_view.key());
    const int next = current < 0 ? 0 : qBound(0, current + delta, int(entries.size()) - 1);
    setViewKey(entries[next].key);
}

void Backend::reveal(qint64 id) {
    m_store->reloadIfChanged();
    if (const Reminder *r = m_store->reminder(id)) {
        const Reminder target = *r;
        if (m_collapsed.remove(target.parentId))
            refresh();
        if (m_model->rowOf(id) < 0)
            setViewKey(target.completed ? QStringLiteral("completed") : "list:" + QString::number(target.listId));
        setSelectedId(id);
    }
    emit raise();
    focusNudgeWindow(QCoreApplication::applicationPid());
}

bool Backend::revealReminder(const QString &database, qlonglong id) {
    const QString here = QFileInfo(m_store->path()).canonicalFilePath();
    if (id < 0 || here.isEmpty() || QFileInfo(database).canonicalFilePath() != here)
        return false;
    m_store->reloadIfChanged();
    if (id && !m_store->reminder(id))
        return false;
    reveal(id);
    return true;
}

QVariantMap Backend::selected() const {
    const Reminder *r = m_store->reminder(m_selectedId);
    if (!r)
        return {};
    const Repeat repeat = Repeat::parse(r->repeat);
    return {
        {"id", r->id},
        {"title", r->title},
        {"notes", r->notes},
        {"url", r->url},
        {"date", Dates::formatEditableDate(r->due)},
        {"dateIso", r->due.isValid() ? r->due.toString(Qt::ISODate) : QString()},
        {"time", Dates::formatTime(r->dueMinutes)},
        {"repeatUnit", repeatUnits.value(repeat.unit)},
        {"repeatInterval", repeat.interval},
        {"repeatEnd", Dates::formatEditableDate(r->repeatEnd)},
        {"early", r->early},
        {"priority", r->priority},
        {"flagged", r->flagged},
        {"tags", r->tags.join(' ')},
        {"listId", r->listId},
        {"sectionId", r->sectionId},
        {"done", r->completed},
        {"subtask", r->parentId != 0},
        {"hasLink", !r->url.trimmed().isEmpty()},
        {"repeatText", ReminderModel::describeRepeat(r->repeat)},
    };
}

// ---- Reminders ---------------------------------------------------------------

double Backend::positionAfter(const Reminder &reminder) const {
    double next = 0;
    bool found = false;
    for (const Reminder &r : m_store->reminders()) {
        if (r.listId != reminder.listId || r.parentId != reminder.parentId || r.id == reminder.id)
            continue;
        if (r.position > reminder.position && (!found || r.position < next)) {
            next = r.position;
            found = true;
        }
    }
    return found ? (reminder.position + next) / 2 : reminder.position + 1;
}

double Backend::positionBefore(const Reminder &reminder) const {
    double previous = 0;
    bool found = false;
    for (const Reminder &r : m_store->reminders()) {
        if (r.listId != reminder.listId || r.parentId != reminder.parentId || r.id == reminder.id)
            continue;
        if (r.position < reminder.position && (!found || r.position > previous)) {
            previous = r.position;
            found = true;
        }
    }
    return found ? (reminder.position + previous) / 2 : reminder.position - 1;
}

// Positions are halved to slot a reminder between two others, which runs out
// of room after some fifty times in the same gap. Then the list is numbered
// afresh, in the order it already has.
void Backend::spread(qint64 listId) {
    QVector<Reminder> all;
    for (const Reminder &r : m_store->reminders())
        if (r.listId == listId)
            all.append(r);
    std::sort(all.begin(), all.end(), [](const Reminder &a, const Reminder &b) {
        return a.position != b.position ? a.position < b.position : a.id < b.id;
    });
    Store::Tx tx(m_store);
    double position = 0;
    for (Reminder &r : all) {
        r.position = ++position;
        m_store->put(r);
    }
}

qint64 Backend::addReminder() {
    if (!m_store->isOpen())
        return 0;
    // One the details pane was filling in is done with first.
    settleDraft();
    m_beforeDraft = m_selectedId;
    m_draftKey = QString("draft:%1").arg(++m_drafts);
    Store::Tx tx(m_store, m_draftKey, Store::Latest);
    if (!tx)
        return 0;
    // A reminder cannot start out finished, or as a search result.
    if (m_view.kind == View::Completed || m_view.kind == View::Search)
        setViewKey("list:" + QString::number(m_store->defaultListId()));

    const QDate today = now().date();
    Reminder r;
    r.draft = true;
    switch (m_view.kind) {
    case View::List: r.listId = m_view.listId; break;
    case View::Today:
    case View::Scheduled: r.due = today; break;
    case View::Flagged: r.flagged = true; break;
    case View::Tag: r.tags = QStringList{m_view.text}; break;
    default: break;
    }
    const qint64 fallback = m_store->defaultListId();
    std::optional<Reminder> selected;
    if (const Reminder *found = m_store->reminder(m_selectedId))
        selected = *found;
    if (!r.listId)
        r.listId = selected ? selected->listId : fallback;
    if (!m_store->list(r.listId))
        r.listId = fallback;
    // A finished reminder takes no new subtasks: beside one of its own, the
    // new reminder goes below it instead, on its own.
    if (const Reminder *parent = selected && selected->parentId ? m_store->reminder(selected->parentId) : nullptr;
        parent && parent->completed && parent->listId == selected->listId)
        selected = *parent;

    // In a list it lands right below the selected reminder, as its
    // sibling; elsewhere at the end of its list.
    if (selected && m_view.isTree() && selected->listId == r.listId) {
        r.parentId = selected->parentId;
        r.sectionId = selected->sectionId;
        r.position = positionAfter(*selected);
        if (r.position - selected->position < 1e-6) {
            spread(r.listId);
            selected = *m_store->reminder(selected->id);
            r.position = positionAfter(*selected);
        }
    } else {
        r.position = m_store->endPosition(r.listId);
    }
    // Known before the transaction ends and the view refreshes, so
    // selecting the new row does not close its undo step.
    m_draftId = m_store->put(r);
    const qint64 id = tx.commit() ? m_draftId : 0;
    emit draftChanged();
    setSelectedId(id);
    return id;
}

void Backend::commitTitle(qint64 id, const QString &text) {
    const Reminder *existing = m_store->reminder(id);
    if (!existing)
        return;
    const QString title = text.simplified();
    if (id != m_draftId) {
        if (!title.isEmpty())
            setTitle(id, title);
        return;
    }
    // Named in the list with nothing, one the details pane is filling in
    // keeps what the pane gave it, and is settled when it is left.
    if (title.isEmpty() && m_draftHeld)
        return;

    const QString key = m_draftKey;
    m_draftId = 0;
    m_draftHeld = false;
    emit draftChanged();
    if (title.isEmpty()) {
        // Nothing was typed, so nothing was added, and nothing is left to undo.
        // The selection goes back to where it was, unless the window has
        // already moved it on.
        const bool selected = m_selectedId == id;
        m_store->discardLast(key);
        m_store->removeEmptyDraft(id);
        if (selected && m_store->reminder(m_beforeDraft) && m_model->rowOf(m_beforeDraft) >= 0)
            setSelectedId(m_beforeDraft);
        return;
    }

    const QuickEntry entry = Dates::parseQuick(title, now());
    {
        Store::Tx tx(m_store, key);
        Reminder r = *existing;
        r.title = entry.title;
        if (entry.due.isValid()) {
            r.due = entry.due;
            r.dueMinutes = entry.minutes;
            r.lateFold = entry.lateFold;
        }
        if (!entry.repeat.isEmpty()) {
            r.repeat = entry.repeat;
            r.repeatEnd = entry.repeatEnd;
        }
        for (const QString &tag : entry.tags)
            if (!r.tags.contains(tag, Qt::CaseInsensitive))
                r.tags << tag;
        if (entry.priority)
            r.priority = entry.priority;
        m_store->put(r);
    }
    m_store->sealUndo();

    // Typing "tomorrow" in Today sends the reminder out of sight; say where.
    const Reminder *saved = m_store->reminder(id);
    if (saved && m_model->rowOf(id) < 0) {
        const TaskList *list = m_store->list(saved->listId);
        emit notice(saved->due.isValid()
                        ? tr("Saved for %1").arg(Dates::formatDue(saved->due, saved->dueMinutes, now().date()))
                        : tr("Saved to %1").arg(list ? list->name : QString()));
    }
}

void Backend::holdDraft(qint64 id) {
    m_draftHeld = id && id == m_draftId;
}

// The details pane was filling in a new reminder, and the selection, the
// view, or the window has left it. With a title or notes it is added, all it
// was given one step to undo; with neither it goes without a trace.
void Backend::settleDraft() {
    if (!m_draftHeld)
        return;
    m_draftHeld = false;
    const qint64 id = std::exchange(m_draftId, 0);
    emit draftChanged();
    const Reminder *r = m_store->reminder(id);
    if (!r)
        return;
    if (!r->title.isEmpty() || !r->notes.trimmed().isEmpty() || !r->url.isEmpty()) {
        if (r->draft) {
            Store::Tx tx(m_store, m_draftKey);
            Reminder saved = *r;
            saved.draft = false;
            m_store->put(saved);
        }
        m_store->sealUndo();
        return;
    }
    m_store->discardLast(m_draftKey);
    m_store->removeEmptyDraft(id);
}

QString Backend::quickPreview(const QString &text) const {
    const QuickEntry entry = Dates::parseQuick(text, now());
    QStringList parts;
    if (entry.due.isValid())
        parts << Dates::formatDue(entry.due, entry.minutes, now().date());
    if (!entry.repeat.isEmpty())
        parts << ReminderModel::describeRepeat(entry.repeat);
    if (entry.repeatEnd.isValid())
        parts << tr("Until %1").arg(Dates::formatDay(entry.repeatEnd, now().date()));
    for (const QString &tag : entry.tags)
        parts << "#" + tag;
    if (entry.priority)
        parts << QString(entry.priority, QChar('!'));
    return parts.join(QStringLiteral(" · "));
}

bool Backend::edit(qint64 id, const QString &what, const std::function<void(Reminder &)> &change) {
    const Reminder *existing = m_store->reminder(id);
    if (!existing)
        return false;
    Reminder r = *existing;
    change(r);
    // Keystrokes in one field of one reminder undo together.
    Store::Tx tx(m_store, stepKey(id, what.isEmpty() ? QString() : QString("%1:%2").arg(what, QString::number(id))));
    m_store->put(r);
    return true;
}

// A change to a reminder still being added is part of adding it, so one
// undo takes it all back.
QString Backend::stepKey(qint64 id, const QString &key) const {
    return id && id == m_draftId ? m_draftKey : key;
}

void Backend::toggleCompleted(qint64 id) {
    const Reminder *r = m_store->reminder(id);
    if (!r)
        return;
    if (r->completed) {
        m_lingering.remove(id);
        m_store->reopen(id);
        return;
    }
    m_lingering.insert(id);
    // Its subtasks still open are completed with it and stay as long; those
    // done before stay out of sight.
    for (qint64 child : m_store->childrenOf(id))
        if (!m_store->reminder(child)->completed)
            m_lingering.insert(child);
    const QDate next = m_store->complete(id, now());
    if (next.isValid()) {
        m_lingering.remove(id);
        emit notice(tr("Done. Next: %1").arg(Dates::formatDay(next, now().date())));
    }
    m_linger.start();
}

void Backend::toggleFlag(qint64 id) {
    edit(id, {}, [](Reminder &r) { r.flagged = !r.flagged; });
}

void Backend::setFlagged(qint64 id, bool flagged) {
    edit(id, {}, [flagged](Reminder &r) { r.flagged = flagged; });
}

void Backend::setPriority(qint64 id, int priority) {
    edit(id, {}, [priority](Reminder &r) { r.priority = qBound(0, priority, 3); });
}

void Backend::setDueIn(qint64 id, int days) {
    const QDate date = now().date().addDays(days);
    edit(id, {}, [date](Reminder &r) {
        r.due = date;
        r.lateFold = false;
        anchor(r);
    });
}

void Backend::clearDue(qint64 id) {
    // Without a date there is nothing to repeat or to alert ahead of.
    edit(id, {}, [](Reminder &r) {
        r.due = QDate();
        r.dueMinutes = -1;
        r.lateFold = false;
        r.repeat.clear();
        r.repeatEnd = QDate();
        r.early = 0;
    });
}

void Backend::remove(qint64 id) {
    if (!m_store->reminder(id))
        return;
    // One the details pane was filling in with nothing in it yet was never
    // added, so it goes without a trace instead.
    if (id == m_draftId && m_draftHeld) {
        settleDraft();
        if (!m_store->reminder(id))
            return;
    }
    if (id == m_draftId) {
        m_draftId = 0;
        emit draftChanged();
    }
    const QString title = m_store->reminder(id)->title;
    m_store->removeReminder(id);
    // A delete that failed has said so through the store, and left it here.
    if (m_store->reminder(id))
        return;
    // One key deletes, so say what went and how to get it back.
    if (!title.isEmpty())
        emit notice(tr("Deleted “%1”. Undo brings it back.").arg(title));
}

void Backend::indent(qint64 id) {
    Store::Tx tx(m_store, stepKey(id), Store::Latest);
    if (!tx)
        return;
    const Reminder *r = m_store->reminder(id);
    if (!r || r->parentId || !m_view.isTree())
        return;
    // The reminder above it becomes its parent. Subtasks go one level deep,
    // so any it has itself come along as siblings.
    qint64 parentId = 0;
    const QVector<Row> &rows = m_model->rows();
    for (int i = m_model->rowOf(id) - 1; i >= 0 && !rows[i].heading; --i) {
        if (rows[i].depth == 0) {
            parentId = rows[i].id;
            break;
        }
    }
    const Reminder *parent = m_store->reminder(parentId);
    // A subtask showing on its own, with its parent out of sight, takes none.
    if (!parent || parent->listId != r->listId || parent->parentId)
        return;

    double position = 0;
    for (qint64 sibling : m_store->childrenOf(parentId))
        position = qMax(position, m_store->reminder(sibling)->position);
    Reminder moved = *r;
    moved.parentId = parentId;
    moved.sectionId = parent->sectionId;
    moved.position = ++position;
    const QVector<qint64> children = m_store->childrenOf(id);
    m_collapsed.remove(parentId);

    // Something left to do under a finished reminder means it is not
    // finished after all.
    bool open = !moved.completed;
    for (qint64 childId : children)
        open = open || !m_store->reminder(childId)->completed;
    const bool reopen = open && parent->completed;
    const QString parentTitle = parent->title;

    {
        if (reopen)
            m_store->reopen(parentId);
        m_store->put(moved);
        for (qint64 childId : children) {
            Reminder child = *m_store->reminder(childId);
            child.parentId = parentId;
            child.sectionId = moved.sectionId;
            child.position = ++position;
            m_store->put(child);
        }
    }
    if (tx.commit() && reopen)
        emit notice(tr("Reopened “%1”: it has a subtask to do").arg(parentTitle));
}

void Backend::outdent(qint64 id) {
    Store::Tx tx(m_store, stepKey(id), Store::Latest);
    if (!tx)
        return;
    const Reminder *r = m_store->reminder(id);
    if (!r || !r->parentId)
        return;
    Reminder moved = *r;
    moved.parentId = 0;
    if (const Reminder *parent = m_store->reminder(r->parentId)) {
        moved.sectionId = parent->sectionId;
        moved.position = positionAfter(*parent);
    } else {
        moved.position = m_store->endPosition(r->listId);
    }
    m_store->put(moved);
}

void Backend::moveReminder(qint64 id, int delta) {
    Store::Tx tx(m_store, {}, Store::Latest);
    if (!tx)
        return;
    const Reminder *found = m_store->reminder(id);
    const TaskList *list = found ? m_store->list(found->listId) : nullptr;
    if (!found || !list || delta == 0)
        return;
    if (!m_view.isTree()) {
        emit notice(tr("Open a list to rearrange its reminders"));
        return;
    }
    if (list->sort != "manual") {
        emit notice(tr("This list is sorted by %1. Choose Manual Order to rearrange it.").arg(sortName(list->sort)));
        return;
    }
    const Reminder r = *found;

    // The reminders it can trade places with are the ones on screen beside
    // it: same parent, and for top-level reminders the same section.
    QVector<qint64> siblings;
    for (const Row &row : m_model->rows()) {
        const Reminder *other = row.heading ? nullptr : m_store->reminder(row.id);
        if (other && other->listId == r.listId && other->parentId == r.parentId &&
            (r.parentId || other->sectionId == r.sectionId))
            siblings.append(other->id);
    }
    const int at = siblings.indexOf(id);
    if (at < 0)
        return;
    const int to = at + (delta < 0 ? -1 : 1);
    Reminder moved = r;

    if (to >= 0 && to < siblings.size()) {
        const Reminder neighbor = *m_store->reminder(siblings[to]);
        moved.position = delta > 0 ? positionAfter(neighbor) : positionBefore(neighbor);
        if (std::abs(moved.position - neighbor.position) < 1e-6) {
            spread(r.listId);
            const Reminder again = *m_store->reminder(neighbor.id);
            moved.position = delta > 0 ? positionAfter(again) : positionBefore(again);
        }
        m_store->put(moved);
        return;
    }

    // Past the edge of a section, it goes on into the one beyond.
    if (r.parentId)
        return;
    QVector<qint64> sectionIds{0};
    for (const Section &section : m_store->sections())
        if (section.listId == r.listId)
            sectionIds.append(section.id);
    const int target = sectionIds.indexOf(r.sectionId) + (delta < 0 ? -1 : 1);
    if (target < 0 || target >= sectionIds.size())
        return;
    moved.sectionId = sectionIds[target];
    if (delta < 0) {
        moved.position = m_store->endPosition(r.listId);
    } else {
        double first = r.position;
        for (const Reminder &other : m_store->reminders())
            if (other.listId == r.listId)
                first = qMin(first, other.position);
        moved.position = first - 1;
    }
    m_store->put(moved);
}

void Backend::moveToList(qint64 id, qint64 listId) {
    Store::Tx tx(m_store, stepKey(id), Store::Latest);
    if (!tx)
        return;
    const Reminder *existing = m_store->reminder(id);
    const TaskList *list = m_store->list(listId);
    if (!existing || !list || existing->listId == listId)
        return;
    const QString name = list->name;
    double position = m_store->endPosition(listId);
    Reminder moved = *existing;
    moved.listId = listId;
    moved.sectionId = 0;
    moved.parentId = 0;
    moved.position = position;
    const QVector<qint64> children = m_store->childrenOf(id);
    {
        m_store->put(moved);
        for (qint64 childId : children) {
            Reminder child = *m_store->reminder(childId);
            child.listId = listId;
            child.sectionId = 0;
            child.position = ++position;
            m_store->put(child);
        }
    }
    if (tx.commit())
        emit notice(tr("Moved to %1").arg(name));
}

void Backend::moveToSection(qint64 id, qint64 sectionId) {
    Store::Tx tx(m_store, stepKey(id), Store::Latest);
    if (!tx)
        return;
    const Reminder *existing = m_store->reminder(id);
    const Section *section = m_store->section(sectionId);
    // Only a section of the reminder's own list will do, whatever a menu offered.
    if (!existing || (sectionId && (!section || section->listId != existing->listId)))
        return;
    Reminder moved = *existing;
    moved.sectionId = sectionId;
    moved.parentId = 0;
    moved.position = m_store->endPosition(moved.listId);
    const QVector<qint64> children = m_store->childrenOf(id);
    m_store->put(moved);
    for (qint64 childId : children) {
        Reminder child = *m_store->reminder(childId);
        child.sectionId = sectionId;
        m_store->put(child);
    }
}

void Backend::toggleCollapsed(qint64 id) {
    if (!m_collapsed.remove(id))
        m_collapsed.insert(id);
    refresh();
}

void Backend::openLink(qint64 id) {
    const Reminder *r = m_store->reminder(id);
    if (!r || r->url.trimmed().isEmpty())
        return;
    if (!QDesktopServices::openUrl(QUrl::fromUserInput(r->url.trimmed())))
        emit notice(tr("Could not open %1").arg(r->url.trimmed()));
}

// ---- The details pane ----------------------------------------------------------

void Backend::setTitle(qint64 id, const QString &text) {
    const QString title = text.simplified();
    if (!title.isEmpty())
        edit(id, "title", [&title](Reminder &r) { r.title = title; });
}

void Backend::setNotes(qint64 id, const QString &text) {
    edit(id, "notes", [&text](Reminder &r) { r.notes = text; });
}

void Backend::setUrl(qint64 id, const QString &text) {
    edit(id, "url", [&text](Reminder &r) { r.url = text.trimmed(); });
}

bool Backend::setDate(qint64 id, const QString &text) {
    if (text.trimmed().isEmpty()) {
        clearDue(id);
        return true;
    }
    const QDate date = Dates::parseDate(text, now().date());
    if (!date.isValid()) {
        emit notice(tr("“%1” is not a date Nudge understands").arg(text.trimmed()));
        return false;
    }
    setDateIso(id, date.toString(Qt::ISODate));
    return true;
}

void Backend::setDateIso(qint64 id, const QString &iso) {
    const QDate date = QDate::fromString(iso, Qt::ISODate);
    if (!date.isValid())
        return;
    edit(id, {}, [date](Reminder &r) {
        if (r.due != date)
            r.lateFold = false;
        r.due = date;
        anchor(r);
    });
}

bool Backend::setTime(qint64 id, const QString &text) {
    if (text.trimmed().isEmpty()) {
        edit(id, {}, [](Reminder &r) { r.dueMinutes = -1; r.lateFold = false; });
        return true;
    }
    const int minutes = Dates::parseTime(text);
    if (minutes < 0) {
        emit notice(tr("“%1” is not a time Nudge understands").arg(text.trimmed()));
        return false;
    }
    const QDateTime moment = now();
    edit(id, {}, [&](Reminder &r) {
        if (r.dueMinutes != minutes)
            r.lateFold = false;
        r.dueMinutes = minutes;
        // A time needs a day: the next one the clock shows that time on.
        if (!r.due.isValid()) {
            const bool ahead = minutes > moment.time().hour() * 60 + moment.time().minute();
            r.due = ahead ? moment.date() : moment.date().addDays(1);
        }
    });
    return true;
}

void Backend::setRepeat(qint64 id, const QString &unit, int interval) {
    Store::Tx tx(m_store, stepKey(id, "repeat:" + QString::number(id)), Store::Latest);
    if (!tx)
        return;
    Repeat repeat;
    repeat.unit = Repeat::Unit(qMax(0, int(repeatUnits.indexOf(unit))));
    repeat.interval = qBound(1, interval, 999);
    const QDate today = now().date();
    edit(id, "repeat", [&](Reminder &r) {
        if (repeat.repeats() && !r.due.isValid())
            r.due = today;
        if (repeat.unit == Repeat::Monthly || repeat.unit == Repeat::Yearly)
            repeat.anchorDay = r.due.day();
        r.repeat = repeat.toString();
        if (!repeat.repeats())
            r.repeatEnd = QDate();
    });
}

bool Backend::setRepeatEnd(qint64 id, const QString &text) {
    QDate date;
    if (!text.trimmed().isEmpty()) {
        date = Dates::parseDate(text, now().date());
        if (!date.isValid()) {
            emit notice(tr("“%1” is not a date Nudge understands").arg(text.trimmed()));
            return false;
        }
    }
    edit(id, {}, [date](Reminder &r) { r.repeatEnd = date; });
    return true;
}

void Backend::setEarly(qint64 id, int minutes) {
    edit(id, {}, [minutes](Reminder &r) { r.early = qMax(0, minutes); });
}

void Backend::setTags(qint64 id, const QString &text) {
    static const QRegularExpression separator("[\\s,]+");
    QStringList tags;
    for (QString tag : text.split(separator, Qt::SkipEmptyParts)) {
        while (tag.startsWith('#'))
            tag.remove(0, 1);
        if (!tag.isEmpty() && !tags.contains(tag, Qt::CaseInsensitive))
            tags << tag;
    }
    edit(id, {}, [&tags](Reminder &r) { r.tags = tags; });
}

// ---- Lists and sections ----------------------------------------------------------

// A list is colored by name, in the theme's own colors, so it changes shade
// with the theme. Lists from before these names keep their "#rrggbb".
QStringList Backend::palette() const {
    return {"blue", "green", "yellow", "orange", "red", "magenta", "cyan", "brown"};
}

QVariantList Backend::lists() const {
    QVariantList result;
    for (const TaskList &list : m_store->lists())
        result.append(QVariantMap{{"id", list.id}, {"name", list.name}, {"color", list.color}});
    return result;
}

QVariantList Backend::sections() const {
    QVariantList result;
    if (m_view.kind != View::List)
        return result;
    for (const Section &section : m_store->sections())
        if (section.listId == m_view.listId)
            result.append(QVariantMap{{"id", section.id}, {"name", section.name}});
    return result;
}

qint64 Backend::addList(const QString &name, const QString &color) {
    const QStringList colors = palette();
    TaskList list;
    list.name = name.simplified().isEmpty() ? tr("New List") : name.simplified();
    list.color = QColor::isValidColorName(color) ? color : colors[m_store->lists().size() % colors.size()];
    list.position = m_store->lists().isEmpty() ? 1 : m_store->lists().last().position + 1;
    const qint64 id = m_store->put(list);
    if (id)
        setViewKey("list:" + QString::number(id));
    return id;
}

void Backend::updateList(qint64 id, const QString &name, const QString &color) {
    const TaskList *existing = m_store->list(id);
    if (!existing)
        return;
    TaskList list = *existing;
    if (!name.simplified().isEmpty())
        list.name = name.simplified();
    if (QColor::isValidColorName(color))
        list.color = color;
    m_store->put(list);
}

void Backend::removeList(qint64 id) {
    const QVector<TaskList> &lists = m_store->lists();
    int at = -1;
    for (int i = 0; i < lists.size(); ++i)
        if (lists[i].id == id)
            at = i;
    if (at < 0)
        return;
    // Deleting the list on screen moves on to the one beside it.
    const bool showing = m_view.kind == View::List && m_view.listId == id;
    const qint64 neighbor = at + 1 < lists.size() ? lists[at + 1].id : at > 0 ? lists[at - 1].id : 0;
    m_store->removeList(id);
    if (showing && neighbor)
        setViewKey("list:" + QString::number(neighbor));
}

void Backend::moveList(qint64 id, int delta) {
    const QVector<TaskList> &lists = m_store->lists();
    int at = -1;
    for (int i = 0; i < lists.size(); ++i)
        if (lists[i].id == id)
            at = i;
    const int to = at + (delta < 0 ? -1 : 1);
    if (at < 0 || to < 0 || to >= lists.size())
        return;
    TaskList moved = lists[at], other = lists[to];
    std::swap(moved.position, other.position);
    if (moved.position == other.position)
        moved.position += delta < 0 ? -0.5 : 0.5;
    Store::Tx tx(m_store);
    m_store->put(moved);
    m_store->put(other);
}

QVariantMap Backend::listInfo(qint64 id) const {
    const TaskList *list = m_store->list(id);
    if (!list)
        return {};
    int total = 0;
    for (const Reminder &r : m_store->reminders())
        total += r.listId == id;
    return {{"id", list->id}, {"name", list->name}, {"color", list->color}, {"total", total}};
}

qint64 Backend::addSection(const QString &name) {
    if (m_view.kind != View::List) {
        emit notice(tr("Sections belong to a list. Open one first."));
        return 0;
    }
    Section section;
    section.listId = m_view.listId;
    section.name = name.simplified().isEmpty() ? tr("New Section") : name.simplified();
    section.position = m_store->sections().isEmpty() ? 1 : m_store->sections().last().position + 1;
    return m_store->put(section);
}

void Backend::renameSection(qint64 id, const QString &name) {
    const Section *existing = m_store->section(id);
    if (!existing || name.simplified().isEmpty())
        return;
    Section section = *existing;
    section.name = name.simplified();
    m_store->put(section);
}

void Backend::removeSection(qint64 id) {
    m_store->removeSection(id);
}

QString Backend::sectionName(qint64 id) const {
    const Section *section = m_store->section(id);
    return section ? section->name : QString();
}

// What Delete Completed takes: the completed reminders the view holds, on
// screen or not (all a list has done, or what Today, a tag, or a search has),
// each with its subtasks. One with a subtask still open stays, so nothing
// left to do loses what it belongs to.
QVector<qint64> Backend::completedInView() const {
    QVector<qint64> held;
    if (m_view.kind == View::List) {
        for (const Reminder &r : m_store->reminders())
            if (r.completed && r.listId == m_view.listId)
                held.append(r.id);
    } else {
        ViewOptions options;
        options.now = now();
        options.showCompleted = true;
        for (const Row &row : buildRows(*m_store, m_view, options))
            if (const Reminder *r = row.heading ? nullptr : m_store->reminder(row.id); r && r->completed)
                held.append(r->id);
    }
    QHash<qint64, QVector<qint64>> children;
    for (const Reminder &r : m_store->reminders())
        if (r.parentId)
            children[r.parentId].append(r.id);
    QSet<qint64> taken;
    for (qint64 id : held) {
        const QVector<qint64> subtasks = children.value(id);
        if (std::any_of(subtasks.cbegin(), subtasks.cend(),
                        [this](qint64 subtask) { return !m_store->reminder(subtask)->completed; }))
            continue;
        taken.insert(id);
        for (qint64 subtask : subtasks)
            taken.insert(subtask);
    }
    QVector<qint64> ids(taken.cbegin(), taken.cend());
    std::sort(ids.begin(), ids.end());
    return ids;
}

int Backend::completedCount() const {
    return completedInView().size();
}

QVariantList Backend::completedIds() const {
    QVariantList ids;
    for (qint64 id : completedInView())
        ids.append(id);
    return ids;
}

void Backend::clearCompleted(const QVariantList &asked) {
    Store::Tx tx(m_store, {}, Store::Latest);
    if (!tx)
        return;
    QSet<qint64> wanted;
    for (const QVariant &id : asked)
        wanted.insert(id.toLongLong());
    QVector<qint64> ids = completedInView();
    ids.removeIf([&wanted](qint64 id) { return !wanted.contains(id); });
    const auto count = m_store->removeReminders(ids);
    if (!tx.commit())
        return;
    if (!count) {
        emit notice(tr("There are no completed reminders to delete"));
        return;
    }
    emit notice(tr("Deleted %n completed reminders", "", count));
}

void Backend::clearCompleted() {
    clearCompleted(completedIds());
}

// ---- Undo, and reminders in the background -----------------------------------------

void Backend::undo() {
    if (!m_store->canUndo()) {
        emit notice(tr("Nothing to undo"));
        return;
    }
    m_lingering.clear();
    m_store->undo();
}

void Backend::redo() {
    if (!m_store->canRedo()) {
        emit notice(tr("Nothing to redo"));
        return;
    }
    m_lingering.clear();
    m_store->redo();
}

bool Backend::backgroundAvailable() const {
    return QFile::exists(m_unitFile);
}

bool Backend::backgroundAsk() const {
    return backgroundAvailable() && !m_backgroundOn && !m_backgroundAsked;
}

void Backend::checkBackground() {
    bool on = false;
    if (backgroundAvailable()) {
        QProcess process;
        process.start(m_systemctl, {"--user", "is-enabled", "nudge.service"});
        if (process.waitForFinished(2000))
            on = process.readAllStandardOutput().trimmed() == "enabled";
        else
            process.kill();
    }
    m_backgroundOn = on;
    emit backgroundChanged();
}

void Backend::setBackground(bool on) {
    if (!backgroundAvailable()) {
        emit notice(tr("Reminders in the background need Nudge installed as a package"));
        return;
    }
    QProcess process;
    process.start(m_systemctl, {"--user", on ? "enable" : "disable", "--now", "nudge.service"});
    if (!process.waitForFinished(8000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString reason = QString::fromUtf8(process.readAllStandardError()).trimmed();
        process.kill();
        emit notice(tr("Could not change reminders in the background: %1").arg(reason));
    } else {
        emit notice(on ? tr("Reminders will arrive even when Nudge is closed")
                       : tr("Reminders will only arrive while Nudge is open"));
    }
    dismissBackgroundAsk();
    checkBackground();
}

void Backend::dismissBackgroundAsk() {
    m_backgroundAsked = true;
    m_store->setMeta("background_asked", "1");
    emit backgroundChanged();
}
