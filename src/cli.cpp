// The command layout follows Hype's src/cli.cpp (MIT, David Heinemeier Hansson).
#include "cli.h"

#include "alerts.h"
#include "dates.h"
#include "recurrence.h"
#include "remindermodel.h"
#include "store.h"
#include "translator.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <functional>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const QStringList priorities{"none", "low", "medium", "high"};

QString reminderTitle(const Reminder &r) {
    return r.title.trimmed().isEmpty() ? QCoreApplication::translate("Store", "Untitled reminder") : r.title;
}

// What is stored can come from an import or another program, so anything in
// it that could steer the terminal (escape sequences, carriage returns, C1
// and bidi controls, line separators) prints as a replacement character.
// JSON escapes these itself.
QString printable(QString text) {
    for (QChar &c : text) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != '\n' && u != '\t') || (u >= 0x7f && u <= 0x9f) || u == 0x2028 || u == 0x2029 ||
            (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069))
            c = QChar::ReplacementCharacter;
    }
    return text;
}

// A title or a name prints on one line, whatever line breaks it holds.
QString oneLine(QString text) { return text.replace('\r', "\\r").replace('\n', "\\n"); }

void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(printable(text))); }
void print(const QJsonObject &object) { fprintf(stdout, "%s", QJsonDocument(object).toJson().constData()); }
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool wantsJson() const { return parser.isSet("json"); }
    QStringList arguments() const { return parser.positionalArguments().mid(1); }
};

// A write the store could not make arrives as Store::failed, and the store
// then holds what the disk does. A command says so instead of reporting
// success.
class Failures {
public:
    explicit Failures(Store &store) {
        QObject::connect(&store, &Store::failed, &m_guard, [this](const QString &why) {
            if (m_message.isEmpty())
                m_message = why;
        });
    }
    bool any() const { return !m_message.isEmpty(); }
    int report(const QString &what) const { return fail(what + ": " + m_message); }

private:
    QObject m_guard; // ends the connection along with this object
    QString m_message;
};

bool ready(Store &store) {
    if (store.isOpen())
        return true;
    print(stderr, "Could not open the reminders: " + store.error());
    return false;
}

QString listNames(const Store &store) {
    QStringList names;
    for (const TaskList &list : store.lists())
        names << oneLine(list.name);
    return names.isEmpty() ? QString("There are no lists yet.") : "Lists: " + names.join(", ");
}

const TaskList *listNamed(const Store &store, const QString &name, QString &error) {
    const TaskList *exact = nullptr, *folded = nullptr;
    int exactCount = 0, foldedCount = 0;
    for (const TaskList &list : store.lists()) {
        if (list.name == name) {
            ++exactCount;
            exact = &list;
        }
        if (list.name.compare(name, Qt::CaseInsensitive) == 0) {
            ++foldedCount;
            folded = &list;
        }
    }
    if (exactCount == 1)
        return exact;
    if (foldedCount == 1)
        return folded;
    error = foldedCount ? QString("More than one list matches “%1”. Rename a list in the window.").arg(oneLine(name))
                        : QString("No list named %1. %2").arg(oneLine(name), listNames(store));
    return nullptr;
}

QString isoTime(int minutes) {
    return QString("%1:%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0'));
}

QJsonValue isoMoment(qint64 seconds) {
    return seconds ? QJsonValue(QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC).toString(Qt::ISODate))
                   : QJsonValue();
}

std::optional<qint64> readMoment(const QJsonValue &value, bool requireOffset = false) {
    if (value.isUndefined() || value.isNull())
        return 0;
    const QDateTime time = QDateTime::fromString(value.toString(), Qt::ISODate);
    // Early exports wrote local completion and creation times. Snoozes have
    // always been exported as UTC instants.
    if (!time.isValid() || (requireOffset && (time.timeSpec() == Qt::LocalTime ||
                                              !value.toString().contains('T', Qt::CaseInsensitive))))
        return std::nullopt;
    return time.toSecsSinceEpoch();
}

QJsonObject toJson(const Store &store, const Reminder &r) {
    const TaskList *list = store.list(r.listId);
    const Section *section = store.section(r.sectionId);
    return {{"id", r.id},
            {"title", reminderTitle(r)},
            {"notes", r.notes},
            {"url", r.url},
            {"list", list ? list->name : QString()},
            {"listId", r.listId},
            {"section", section ? QJsonValue(section->name) : QJsonValue()},
            {"sectionId", section ? QJsonValue(section->id) : QJsonValue()},
            {"parent", r.parentId ? QJsonValue(r.parentId) : QJsonValue()},
            {"due", r.due.isValid() ? QJsonValue(r.due.toString(Qt::ISODate)) : QJsonValue()},
            {"time", r.dueMinutes >= 0 ? QJsonValue(isoTime(r.dueMinutes)) : QJsonValue()},
            {"lateFold", r.lateFold},
            {"repeat", r.repeat.isEmpty() ? QJsonValue() : QJsonValue(r.repeat)},
            {"repeatEnd", r.repeatEnd.isValid() ? QJsonValue(r.repeatEnd.toString(Qt::ISODate)) : QJsonValue()},
            {"early", r.early},
            {"priority", priorities.value(r.priority, "none")},
            {"flagged", r.flagged},
            {"tags", QJsonArray::fromStringList(r.tags)},
            {"completed", r.completed},
            {"completedAt", isoMoment(r.completedAt)},
            {"createdAt", isoMoment(r.createdAt)},
            {"snoozeUntil", isoMoment(r.snoozeUntil)}};
}

QString toText(const Store &store, const Reminder &r) {
    QStringList parts{QString("%1").arg(r.id, 5), r.completed ? "[x]" : "[ ]",
                      QString(r.parentId ? "  " : "") + reminderTitle(r)};
    if (r.due.isValid())
        parts << r.due.toString(Qt::ISODate) + (r.dueMinutes >= 0 ? " " + isoTime(r.dueMinutes) : QString());
    if (!r.repeat.isEmpty())
        parts << "(" + r.repeat + ")";
    if (const TaskList *list = store.list(r.listId))
        parts << "@" + list->name;
    if (r.priority)
        parts << QString(r.priority, QChar('!'));
    if (r.flagged)
        parts << "flagged";
    for (const QString &tag : r.tags)
        parts << "#" + tag;
    return oneLine(parts.join("  "));
}

// "daily", "weekly", "every 2 weeks", "every monday" as a rule, or the rule
// itself. A repeat needs a date, the one it has or today, and a rule that
// names a day moves it to the next such day, as quick entry does, so "every
// monday" falls on Mondays.
bool readRepeat(const QString &text, const QDate &today, Reminder &r) {
    const QString word = text.trimmed().toLower();
    if (word == "none" || word == "never") {
        r.repeat.clear();
        r.repeatEnd = QDate();
        return true;
    }
    QDate date = r.due.isValid() ? r.due : today;
    Repeat repeat = Repeat::parse(word.contains(':') ? word : word + ":1");
    if (!repeat.repeats()) {
        const QuickEntry entry = Dates::parseQuick("x " + word, QDateTime(date, QTime(0, 0)));
        if (entry.title != "x")
            return false;
        repeat = Repeat::parse(entry.repeat);
        if (entry.due.isValid())
            date = entry.due;
        if (entry.repeatEnd.isValid())
            r.repeatEnd = entry.repeatEnd;
    }
    if (!repeat.repeats())
        return false;
    if (repeat.unit == Repeat::Weekdays)
        date = Repeat::firstWorkday(date);
    const bool moved = r.due != date;
    if (moved)
        r.lateFold = false;
    r.repeat = repeat.anchored(date, moved ? QString() : r.repeat);
    r.due = date;
    return true;
}

// Tags as the window reads them: words apart by spaces or commas, without
// their #. The store keeps tags apart by spaces, so no tag can hold one.
QStringList readTags(const QString &text) {
    static const QRegularExpression separator("[\\s,]+");
    QStringList tags;
    for (QString tag : text.split(separator, Qt::SkipEmptyParts)) {
        while (tag.startsWith('#'))
            tag.remove(0, 1);
        if (!tag.isEmpty() && !tags.contains(tag, Qt::CaseInsensitive))
            tags << tag;
    }
    return tags;
}

// The options add and edit share, by the rules the window keeps. Returns an
// error, or nothing.
QString applyOptions(const QCommandLineParser &parser, Store &store, Reminder &r) {
    const QDateTime now = QDateTime::currentDateTime();
    const QDate today = now.date();
    if (parser.isSet("list")) {
        QString error;
        const TaskList *list = listNamed(store, parser.value("list"), error);
        if (!list)
            return error;
        if (list->id != r.listId) {
            r.listId = list->id;
            r.sectionId = 0;
            r.parentId = 0;
            r.position = store.endPosition(list->id);
        }
    }
    if (parser.isSet("notes"))
        r.notes = parser.value("notes");
    if (parser.isSet("url"))
        r.url = parser.value("url").trimmed();
    if (parser.isSet("due")) {
        const QString text = parser.value("due");
        if (text == "none") {
            // Without a date there is nothing to repeat or to alert ahead of.
            r.due = QDate();
            r.lateFold = false;
            r.dueMinutes = -1;
            r.repeat.clear();
            r.repeatEnd = QDate();
            r.early = 0;
        } else {
            const QDate date = Dates::parseDate(text, today);
            if (!date.isValid())
                return QString("Could not read %1 as a date. Try 2026-12-24, tomorrow, or \"next friday\".").arg(text);
            if (r.due != date) {
                r.repeat = Repeat::parse(r.repeat).anchored(date);
                r.lateFold = false;
            }
            r.due = date;
        }
    }
    if (parser.isSet("time")) {
        const QString text = parser.value("time");
        if (text == "none") {
            r.lateFold = false;
            r.dueMinutes = -1;
        } else {
            const int minutes = Dates::parseTime(text);
            if (minutes < 0)
                return QString("Could not read %1 as a time. Try 17:30 or 5pm.").arg(text);
            if (r.dueMinutes != minutes)
                r.lateFold = false;
            r.dueMinutes = minutes;
            // A time needs a day: the next one the clock shows that time on.
            if (!r.due.isValid())
                r.due = Dates::nextClockDate(minutes, now, r.lateFold);
        }
    }
    if (parser.isSet("repeat") && !readRepeat(parser.value("repeat"), today, r))
        return QString("Could not read %1 as a repeat. Try daily, weekdays, weekly, monthly, yearly, "
                       "\"every 2 weeks\", or none.")
            .arg(parser.value("repeat"));
    if (parser.isSet("priority")) {
        const int priority = priorities.indexOf(parser.value("priority").toLower());
        if (priority < 0)
            return QString("Priority is one of: %1.").arg(priorities.join(", "));
        r.priority = priority;
    }
    if (parser.isSet("flag"))
        r.flagged = true;
    for (const QString &tag : readTags(parser.values("tag").join(' ')))
        if (!r.tags.contains(tag, Qt::CaseInsensitive))
            r.tags << tag;
    return {};
}

void addEditOptions(QCommandLineParser &parser) {
    parser.addOption({"list", "The list it belongs to", "name"});
    parser.addOption({"notes", "Notes", "text"});
    parser.addOption({"url", "A link", "url"});
    parser.addOption({"due", "Due date: 2026-12-24, tomorrow, \"next friday\", or none", "date"});
    parser.addOption({"time", "Due time: 17:30, 5pm, or none", "time"});
    parser.addOption({"repeat", "daily, weekdays, weekly, monthly, yearly, \"every 2 weeks\", or none", "rule"});
    parser.addOption({"priority", "none, low, medium, or high", "level"});
    parser.addOption({"flag", "Flag it"});
    parser.addOption({"tag", "Add a tag; may be given more than once", "tag"});
}

int report(const Command &command, const Store &store, qint64 id, const QString &verb) {
    const Reminder *r = store.reminder(id);
    if (!r)
        return fail("The reminder could not be saved: " + store.error());
    if (command.wantsJson())
        print(toJson(store, *r));
    else
        print(stdout, QString("%1 %2: %3").arg(verb, QString::number(id), oneLine(r->title)));
    return 0;
}

int add(const QStringList &arguments) {
    Command command("add", "Add a reminder. Dates, times, #tags and ! priority in the text are understood: "
                           "\"Call the dentist tomorrow at 9 #health !!\".");
    command.parser.addPositionalArgument("text", "What to be reminded of", "<text>");
    addEditOptions(command.parser);
    command.parser.addOption({"parent", "Make it a subtask of this reminder", "id"});
    command.parser.addOption({"literal", "Take the text as the title, word for word"});
    command.json();
    command.parser.process(arguments);
    const bool literal = command.parser.isSet("literal");
    const QString text = command.arguments().join(' ');
    if (text.trimmed().isEmpty())
        return fail("Say what to be reminded of: nudge add \"Water the plants every friday\"");

    Store store;
    if (!ready(store))
        return 1;
    Failures failures(store);
    Store::Tx tx(&store, {}, Store::Latest);
    if (!tx)
        return failures.report("The reminder could not be saved");
    Reminder r;
    QuickEntry entry;
    entry.title = text;
    if (!literal)
        entry = Dates::parseQuick(text, QDateTime::currentDateTime());
    r.title = entry.title;
    r.due = entry.due;
    r.dueMinutes = entry.minutes;
    r.lateFold = entry.lateFold;
    r.repeat = entry.repeat;
    r.repeatEnd = entry.repeatEnd;
    // A tag written twice is one tag, as quick entry in the window keeps it.
    // Not readTags, which would split one such as #home,office.
    for (const QString &tag : entry.tags)
        if (!r.tags.contains(tag, Qt::CaseInsensitive))
            r.tags << tag;
    r.priority = entry.priority;
    r.listId = store.defaultListId();
    r.position = store.endPosition(r.listId);

    if (command.parser.isSet("parent")) {
        const Reminder *parent = store.reminder(command.parser.value("parent").toLongLong());
        if (!parent)
            return fail("There is no reminder " + command.parser.value("parent") + ".");
        if (parent->parentId)
            return fail("Subtasks go one level deep, and that reminder is a subtask itself.");
        r.parentId = parent->id;
        r.listId = parent->listId;
        r.sectionId = parent->sectionId;
        r.position = store.endPosition(r.listId);
    }
    // Read before the options, which let go of the parent when --list names
    // another list: a subtask lives in its parent's list.
    const qint64 parentId = r.parentId, listId = r.listId;
    const QString error = applyOptions(command.parser, store, r);
    if (!error.isEmpty())
        return fail(error);
    if (entry.weekday && !command.parser.isSet("repeat") && r.due.isValid()) {
        const QDate first = Repeat::onWeekday(r.due, entry.weekday);
        if (first != r.due)
            r.lateFold = false;
        r.due = first;
    }
    if (r.due.isValid() && r.repeatEnd.isValid() && r.due > r.repeatEnd)
        return fail("The repeat would start after it ends.");
    if (parentId && r.listId != listId)
        return fail("A subtask lives in its parent's list; leave out --list.");

    // A done reminder has no open subtasks, so adding one reopens it.
    const Reminder *parent = store.reminder(parentId);
    const bool reopen = parent && parent->completed;
    if (reopen)
        store.reopen(parentId);
    const qint64 id = store.put(r);
    if (!tx.commit() || failures.any())
        return failures.report("The reminder could not be saved");
    const int result = report(command, store, id, "Added");
    if (result == 0 && reopen && !command.wantsJson())
        print(stdout, QString("Reopened %1: %2, which has something to do again")
                          .arg(QString::number(parentId), oneLine(store.reminder(parentId)->title)));
    return result;
}

int edit(const QStringList &arguments) {
    Command command("edit", "Change a reminder.");
    command.parser.addPositionalArgument("id", "The reminder", "<id>");
    command.parser.addOption({"title", "A new title", "text"});
    addEditOptions(command.parser);
    command.parser.addOption({"unflag", "Take the flag off"});
    command.parser.addOption({"tags", "Replace the tags with these, separated by spaces or commas", "tags"});
    command.json();
    command.parser.process(arguments);

    Store store;
    if (!ready(store))
        return 1;
    Failures failures(store);
    Store::Tx tx(&store, {}, Store::Latest);
    if (!tx)
        return failures.report("The change could not be saved");
    const qint64 id = command.arguments().value(0).toLongLong();
    const Reminder *existing = store.reminder(id);
    if (!existing)
        return fail(command.arguments().isEmpty() ? QString("Name the reminder to change by its id.")
                                                   : "There is no reminder " + command.arguments().first() + ".");
    Reminder r = *existing;
    if (command.parser.isSet("title")) {
        if (command.parser.value("title").simplified().isEmpty())
            return fail("A reminder needs a title.");
        r.title = command.parser.value("title").simplified();
    }
    if (command.parser.isSet("tags"))
        r.tags = readTags(command.parser.value("tags"));
    const qint64 listId = r.listId;
    const QString error = applyOptions(command.parser, store, r);
    if (!error.isEmpty())
        return fail(error);
    // Only edit has --unflag; add has nothing to take off.
    if (command.parser.isSet("unflag"))
        r.flagged = false;

    store.put(r, r.lateFold);
    if (r.listId != listId) {
        // Its subtasks move with it.
        for (qint64 childId : store.childrenOf(id)) {
            Reminder child = *store.reminder(childId);
            child.listId = r.listId;
            child.sectionId = 0;
            store.put(child);
        }
    }
    if (!tx.commit() || failures.any())
        return failures.report("The change could not be saved");
    return report(command, store, id, "Changed");
}

// Called with the write lock held. A final parent completes its open children
// itself. A continuing parent resets its checklist after its children finish.
bool completeSelected(Store &store, const QVector<qint64> &ids, const QHash<qint64, Reminder> &selected,
                      QSet<qint64> &acted) {
    const auto now = QDateTime::currentDateTime();
    QVector<qint64> children, parents;
    QSet<qint64> finalParents, parentsOfSelected;
    for (const Reminder &r : selected)
        if (r.parentId)
            parentsOfSelected.insert(r.parentId);
    for (qint64 id : ids) {
        const auto offered = selected.constFind(id);
        const Reminder *r = store.reminder(id);
        if (!r || r->occurrence() != offered->occurrence() || r->completed != offered->completed ||
            r->draft != offered->draft || r->parentId != offered->parentId) {
            // Skipping just one member could let its selected parent complete
            // the next occurrence, or a child check off the next checklist.
            if (parentsOfSelected.contains(id) || selected.contains(offered->parentId) ||
                (r && selected.contains(r->parentId)))
                return false;
            continue;
        }
        if (r->completed || r->draft)
            continue;
        if (r->parentId) {
            children << id;
        } else {
            parents << id;
            if (!r->nextOccurrence(now).isValid())
                finalParents.insert(id);
        }
    }
    for (qint64 id : children + parents) {
        if (!finalParents.contains(store.reminder(id)->parentId)) {
            bool completed = false;
            store.complete(id, now, &completed);
            if (!completed)
                continue;
        }
        acted.insert(id);
    }
    return true;
}

// done, reopen, and delete all take one or more ids, and do nothing unless
// every one of them exists. An id named twice is acted on once: done twice
// would skip a repeating reminder's next date.
int each(const QStringList &arguments, const QString &name, const QString &description, const QString &verb) {
    Command command(name, description);
    command.parser.addPositionalArgument("id", "One or more reminders", "<id>...");
    command.json();
    command.parser.process(arguments);
    if (command.arguments().isEmpty())
        return fail(QString("Name the reminders by id: nudge %1 12 15").arg(name));

    Store store;
    if (!ready(store))
        return 1;
    QVector<qint64> ids;
    QHash<qint64, Reminder> selected;
    for (const QString &word : command.arguments()) {
        const qint64 id = word.toLongLong();
        const Reminder *r = store.reminder(id);
        if (!r)
            return fail("There is no reminder " + word + ". Nothing was changed.");
        if (!selected.contains(id)) {
            ids << id;
            selected.insert(id, *r);
        }
    }
    Failures failures(store);
    Store::Tx tx(&store, {}, Store::Latest);
    if (!tx)
        return failures.report("Nothing was changed");
    QSet<qint64> acted;
    if (name == "done") {
        if (!completeSelected(store, ids, selected, acted))
            return fail("A selected reminder changed before completion. Nothing was changed.");
    } else if (name == "delete") {
        for (qint64 id : std::as_const(ids))
            if (!store.reminder(id))
                return fail("There is no reminder " + QString::number(id) + ". Nothing was changed.");
        for (qint64 id : std::as_const(ids)) {
            if (store.reminder(id))
                store.removeReminder(id);
            acted.insert(id); // a selected parent may already have removed it
        }
    } else {
        for (qint64 id : std::as_const(ids))
            if (store.reopen(id))
                acted.insert(id);
    }
    if (!tx.commit() || failures.any())
        return failures.report("Nothing was changed");
    QJsonArray changed;
    QStringList lines;
    for (qint64 id : std::as_const(ids)) {
        const QString title = selected[id].title;
        const Reminder *r = store.reminder(id);
        const QString result = acted.contains(id) ? verb : !r ? "No longer there"
                             : name == "reopen" ? "Already open" : "Already completed or moved on";
        lines << QString("%1 %2: %3").arg(result, QString::number(id), oneLine(title));
        if (r)
            changed << toJson(store, *r);
        else
            changed << QJsonObject{{"id", id}, {"title", title}, {"deleted", true}};
    }
    if (command.wantsJson())
        print({{"reminders", changed}});
    else
        for (const QString &line : lines)
            print(stdout, line);
    return 0;
}

int show(const QStringList &arguments) {
    Command command("show", "Show one reminder with everything it holds.");
    command.parser.addPositionalArgument("id", "The reminder", "<id>");
    command.json();
    command.parser.process(arguments);
    Store store;
    if (!ready(store))
        return 1;
    const Reminder *r = store.reminder(command.arguments().value(0).toLongLong());
    if (!r)
        return fail(command.arguments().isEmpty() ? QString("Name the reminder by its id.")
                                                   : "There is no reminder " + command.arguments().first() + ".");
    QJsonObject object = toJson(store, *r);
    QJsonArray subtasks;
    for (qint64 childId : store.childrenOf(r->id))
        subtasks << toJson(store, *store.reminder(childId));
    object.insert("subtasks", subtasks);
    if (command.wantsJson()) {
        print(object);
        return 0;
    }
    print(stdout, toText(store, *r));
    if (!r->notes.isEmpty())
        print(stdout, "       " + QString(r->notes).replace("\r\n", "\n").replace('\n', "\n       "));
    if (!r->url.isEmpty())
        print(stdout, "       " + oneLine(r->url));
    for (qint64 childId : store.childrenOf(r->id))
        print(stdout, toText(store, *store.reminder(childId)));
    return 0;
}

int list(const QStringList &arguments) {
    Command command("list", "List reminders. Without a view, everything that is not done.");
    command.parser.addPositionalArgument("view", "today, scheduled, all, flagged, or completed", "[view]");
    command.parser.addOption({"list", "Only this list", "name"});
    command.parser.addOption({"tag", "Only reminders with this tag", "tag"});
    command.parser.addOption({"search", "Only reminders that mention this", "text"});
    command.json();
    command.parser.process(arguments);

    Store store;
    if (!ready(store))
        return 1;
    const bool named = !command.arguments().isEmpty();
    const QString name = command.arguments().value(0, "all");
    View view = View::fromKey(name);
    if (view.key() != name)
        return fail("Views are: today, scheduled, all, flagged, completed.");
    // --list, --tag and --search each narrow what is listed. Without a view
    // named, the first of them is the view, as the window shows it (a list
    // with its sections); with one, "list today --list Home" is what is due
    // today in Home.
    QVector<View> filters;
    if (command.parser.isSet("list")) {
        QString error;
        const TaskList *found = listNamed(store, command.parser.value("list"), error);
        if (!found)
            return fail(error);
        filters << View{View::List, found->id, {}};
    }
    if (command.parser.isSet("tag")) {
        QString tag = command.parser.value("tag");
        while (tag.startsWith('#'))
            tag.remove(0, 1);
        filters << View{View::Tag, 0, tag};
    }
    if (command.parser.isSet("search"))
        filters << View{View::Search, 0, command.parser.value("search").simplified()};
    if (!named && !filters.isEmpty())
        view = filters.takeFirst();

    ViewOptions options;
    options.now = QDateTime::currentDateTime();
    // What each filter holds, done or not: the view says whether done ones show.
    std::optional<QSet<qint64>> allowed;
    for (const View &filter : std::as_const(filters)) {
        ViewOptions everything = options;
        everything.showCompleted = true;
        QSet<qint64> held;
        for (const Row &row : buildRows(store, filter, everything))
            if (!row.heading)
                held.insert(row.id);
        allowed = allowed ? *allowed & held : held;
    }
    QJsonArray found;
    for (const Row &row : buildRows(store, view, options)) {
        const Reminder *r = row.heading ? nullptr : store.reminder(row.id);
        if (!r || (allowed && !allowed->contains(r->id)))
            continue;
        if (command.wantsJson())
            found << toJson(store, *r);
        else
            print(stdout, toText(store, *r));
    }
    if (command.wantsJson())
        print({{"view", view.key()}, {"reminders", found}});
    return 0;
}

int lists(const QStringList &arguments) {
    Command command("lists", "Show the lists, or: lists add <name>, lists delete <name>.");
    command.parser.addPositionalArgument("action", "add or delete", "[add|delete]");
    command.parser.addPositionalArgument("name", "The list", "[name]");
    command.parser.addOption({"color", "The color of a new list: red, orange, yellow, green, cyan, blue, magenta, "
                                       "brown (the theme's own), or #RRGGBB",
                              "color"});
    command.parser.addOption({"force", "Delete a list even when it holds reminders"});
    command.json();
    command.parser.process(arguments);

    Store store;
    if (!ready(store))
        return 1;
    const QString action = command.arguments().value(0);
    const QString givenName = command.arguments().mid(1).join(' ');
    auto count = [&](qint64 listId, bool openOnly) {
        int total = 0;
        for (const Reminder &r : store.reminders())
            total += r.listId == listId && !(openOnly && r.completed);
        return total;
    };

    if (action == "add") {
        const QString name = givenName.simplified();
        if (name.isEmpty())
            return fail("Name the list: nudge lists add Groceries");
        Failures failures(store);
        Store::Tx tx(&store, {}, Store::Latest);
        if (!tx)
            return failures.report("The list could not be saved");
        if (std::any_of(store.lists().cbegin(), store.lists().cend(), [&](const TaskList &list) {
                return list.name.compare(name, Qt::CaseInsensitive) == 0;
            }))
            return fail("There already is a list named " + name + ".");
        static const QRegularExpression hex("^#[0-9a-fA-F]{6}$");
        static const QStringList named{"red", "orange", "yellow", "green", "cyan", "blue", "magenta", "brown"};
        const QString color = command.parser.value("color").toLower();
        if (command.parser.isSet("color") && !named.contains(color) && !hex.match(color).hasMatch())
            return fail("A color is one of the theme's (" + named.join(", ") + ") or looks like #60a5fa.");
        TaskList created;
        created.name = name;
        created.color = color.isEmpty() ? QString("blue") : color;
        created.position = store.lists().isEmpty() ? 1 : store.lists().last().position + 1;
        created.id = store.put(created);
        if (!tx.commit() || failures.any() || !created.id)
            return failures.report("The list could not be saved");
        if (command.wantsJson())
            print({{"id", created.id}, {"name", created.name}, {"color", created.color}});
        else
            print(stdout, "Added the list " + created.name);
        return 0;
    }
    if (action == "delete") {
        Failures failures(store);
        Store::Tx tx(&store, {}, Store::Latest);
        if (!tx)
            return failures.report("The list could not be deleted");
        QString error;
        const TaskList *doomed = listNamed(store, givenName, error);
        if (!doomed)
            return fail(error);
        const int holding = count(doomed->id, false);
        if (holding && !command.parser.isSet("force"))
            return fail(QString("%1 holds %2 reminders. Add --force to delete them with it.")
                            .arg(oneLine(doomed->name), QString::number(holding)));
        const QString removed = doomed->name;
        store.removeList(doomed->id);
        if (!tx.commit() || failures.any())
            return failures.report("The list could not be deleted");
        if (command.wantsJson())
            print({{"deleted", removed}, {"reminders", holding}});
        else
            print(stdout, "Deleted the list " + oneLine(removed));
        return 0;
    }
    if (!action.isEmpty())
        return fail("Use nudge lists, nudge lists add <name>, or nudge lists delete <name>.");

    QJsonArray all;
    for (const TaskList &entry : store.lists()) {
        const int openCount = count(entry.id, true);
        all << QJsonObject{{"id", entry.id}, {"name", entry.name}, {"color", entry.color}, {"open", openCount},
                           {"total", count(entry.id, false)}};
        if (!command.wantsJson())
            print(stdout, QString("%1  %2").arg(openCount, 4).arg(oneLine(entry.name)));
    }
    if (command.wantsJson())
        print({{"lists", all}});
    return 0;
}

int exportAll(const QStringList &arguments) {
    Command command("export", "Write every list and reminder as JSON, to a file or to standard output.");
    command.parser.addPositionalArgument("file", "Where to write", "[file]");
    command.json();
    command.parser.process(arguments);
    Store store;
    if (!ready(store))
        return 1;
    const QString path = command.arguments().value(0);
    if (!path.isEmpty() && Store::isDatabaseFile(store.path(), path))
        return fail("Choose a backup path separate from the reminders database.");
    QJsonArray lists, reminders;
    for (const TaskList &entry : store.lists()) {
        // Names, as every export has had them, and beside them the ids, which
        // tell apart two lists or two sections that are called the same.
        QJsonArray sections, sectionIds;
        for (const Section &section : store.sections())
            if (section.listId == entry.id) {
                sections << section.name;
                sectionIds << section.id;
            }
        lists << QJsonObject{{"id", entry.id},       {"name", entry.name},     {"color", entry.color},
                             {"sort", entry.sort},   {"sections", sections},   {"sectionIds", sectionIds}};
    }
    QVector<const Reminder *> ordered;
    QSet<qint64> namedParents;
    for (const Reminder &r : store.reminders())
        if (!r.draft && r.hasContent() && r.parentId)
            namedParents.insert(r.parentId);
    for (const Reminder &r : store.reminders())
        if ((!r.draft && r.hasContent()) || namedParents.contains(r.id))
            ordered << &r;
    std::sort(ordered.begin(), ordered.end(), [](const Reminder *a, const Reminder *b) {
        return a->listId != b->listId ? a->listId < b->listId
               : a->position != b->position ? a->position < b->position
                                            : a->id < b->id;
    });
    for (const Reminder *r : ordered)
        reminders << toJson(store, *r);
    const QJsonObject document{{"nudge", 1},
                               {"exportedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                               {"lists", lists},
                               {"reminders", reminders}};
    // To standard output it is JSON, whether --json says so or not.
    if (path.isEmpty()) {
        print(document);
        return 0;
    }
    // Written beside the target and renamed over it, so a failure halfway
    // leaves any earlier export whole.
    // Readable by its owner alone, as the reminders it holds are, whatever
    // file it replaces.
    QSaveFile file(path);
    const QByteArray json = QJsonDocument(document).toJson();
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(json) != json.size() || !file.commit())
        return fail("Could not write " + path + ": " + file.errorString());
    if (command.wantsJson())
        print({{"exported", int(reminders.size())}, {"file", QFileInfo(path).absoluteFilePath()}});
    else
        print(stdout, QString("Exported %1 reminders to %2").arg(reminders.size()).arg(path));
    return 0;
}

std::optional<Reminder> reminderFromBackup(const QJsonObject &entry, const QDate &today) {
    Reminder r;
    r.title = entry.value("title").toString();
    r.notes = entry.value("notes").toString();
    r.url = entry.value("url").toString();
    r.due = QDate::fromString(entry.value("due").toString(), Qt::ISODate);
    r.repeatEnd = QDate::fromString(entry.value("repeatEnd").toString(), Qt::ISODate);
    const QTime time = QTime::fromString(entry.value("time").toString(), "HH:mm");
    auto valid = [&](const QString &key, const QString &formatted) {
        const QJsonValue value = entry.value(key);
        return value.isNull() || value.isUndefined() || (!formatted.isEmpty() && value.toString() == formatted);
    };
    if (!valid("due", r.due.toString(Qt::ISODate)) || !valid("repeatEnd", r.repeatEnd.toString(Qt::ISODate)) ||
        !valid("time", time.toString("HH:mm")))
        return std::nullopt;
    const bool hadDate = r.due.isValid();
    r.repeat = Repeat::parse(entry.value("repeat").toString()).toString();
    // Start a dateless legacy series only while it still has an occurrence.
    if (!hadDate && !r.repeat.isEmpty()) {
        const QDate start = r.repeat == "weekdays" ? Repeat::firstWorkday(today) : today;
        if (!r.repeatEnd.isValid() || start <= r.repeatEnd)
            r.due = start;
    }
    r.dueMinutes = r.due.isValid() && time.isValid() ? time.hour() * 60 + time.minute() : -1;
    r.lateFold = hadDate && r.dueMinutes >= 0 && entry.value("lateFold").toBool();
    r.early = entry.value("early").toInt();
    r.priority = qMax(0, int(priorities.indexOf(entry.value("priority").toString())));
    r.flagged = entry.value("flagged").toBool();
    for (const QJsonValue &tag : entry.value("tags").toArray())
        if (const QString text = tag.toString(); !text.trimmed().isEmpty())
            r.tags << text;
    r.completed = entry.value("completed").toBool();
    const auto completed = readMoment(entry.value("completedAt")), created = readMoment(entry.value("createdAt")),
               snooze = readMoment(entry.value("snoozeUntil"), true);
    if (!completed || !created || !snooze)
        return std::nullopt;
    r.completedAt = *completed;
    r.createdAt = *created;
    r.snoozeUntil = r.completed || !hadDate ? 0 : *snooze;
    return r;
}

int importAll(const QStringList &arguments) {
    Command command("import", "Add the lists and reminders of an export. Nothing already here is changed.");
    command.parser.addPositionalArgument("file", "An export made by nudge export", "<file>");
    command.json();
    command.parser.process(arguments);
    const QString path = command.arguments().value(0);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return fail(path.isEmpty() ? QString("Name the file to import.") : "Could not read " + path + ".");
    QJsonParseError problem;
    const QJsonObject document = QJsonDocument::fromJson(file.readAll(), &problem).object();
    if (problem.error != QJsonParseError::NoError || !document.value("reminders").isArray() ||
        (document.contains("lists") && !document.value("lists").isArray()))
        return fail(path + " is not a Nudge export.");

    const QJsonArray entries = document.value("reminders").toArray();
    struct Imported {
        QJsonObject entry;
        Reminder reminder;
        qint64 oldId, oldParent;
    };
    QVector<Imported> imported;
    imported.reserve(entries.size());
    const QDate today = QDate::currentDate();
    QHash<qint64, qint64> parentOf;
    for (const QJsonValue &value : entries) {
        if (!value.isObject())
            return fail("A reminder in the backup is invalid.");
        const QJsonObject entry = value.toObject();
        const QJsonValue idValue = entry.value("id"), parentValue = entry.value("parent");
        const qint64 id = idValue.toInteger(-1);
        const qint64 parent = parentValue.isUndefined() || parentValue.isNull() ? 0 : parentValue.toInteger(-1);
        if ((!idValue.isUndefined() && !idValue.isNull() && id <= 0) || parent < 0 ||
            (id > 0 && parentOf.contains(id)))
            return fail("The backup has invalid or duplicate reminder ids.");
        if (id > 0)
            parentOf.insert(id, parent);
        const auto reminder = reminderFromBackup(entry, today);
        if (!reminder)
            return fail("A reminder in the backup has an invalid date or time.");
        imported.append({entry, *reminder, qMax<qint64>(id, 0), parent});
    }
    QSet<qint64> neededParents;
    for (const Imported &row : std::as_const(imported)) {
        if (row.oldParent && parentOf.value(row.oldParent, -1) != 0)
            return fail("A subtask in the backup does not name a top-level reminder.");
        if (row.oldParent && row.reminder.hasContent())
            neededParents.insert(row.oldParent);
    }
    QSet<QString> incomingNames;
    QSet<qint64> declaredLists;
    struct SectionName {
        qint64 listId;
        QString list, section;
    };
    QHash<qint64, SectionName> declaredSections;
    for (const QJsonValue &value : document.value("lists").toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value("name").toString().trimmed().isEmpty() ||
            (entry.contains("sections") && !entry.value("sections").isArray()))
            return fail("The backup has an invalid list.");
        const QJsonValue idValue = entry.value("id");
        if (!idValue.isUndefined() && !idValue.isNull()) {
            const qint64 id = idValue.toInteger();
            if (id <= 0 || declaredLists.contains(id))
                return fail("The backup has invalid or duplicate list ids.");
            declaredLists.insert(id);
        }
        const QString listName = entry.value("name").toString();
        incomingNames.insert(listName);
        if (entry.contains("sectionIds")) {
            const QJsonValue ids = entry.value("sectionIds");
            const QJsonArray sectionIds = ids.toArray(), names = entry.value("sections").toArray();
            if (!ids.isArray() || sectionIds.size() != names.size())
                return fail("The backup has invalid or duplicate section ids.");
            for (qsizetype i = 0; i < names.size(); ++i) {
                const qint64 id = sectionIds.at(i).toInteger();
                const QString name = names.at(i).toString();
                if (id <= 0 || name.isEmpty() || declaredSections.contains(id))
                    return fail("The backup has invalid or duplicate section ids.");
                declaredSections.insert(id, {idValue.toInteger(), listName, name});
            }
        }
    }
    for (const Imported &row : std::as_const(imported)) {
        const auto id = row.entry.value("sectionId");
        if (id.isNull() || id.isUndefined())
            continue;
        const auto section = declaredSections.constFind(id.toInteger());
        const auto listId = row.entry.value("listId").toInteger();
        const auto listName = row.entry.value("list"), sectionName = row.entry.value("section");
        if (section == declaredSections.cend() || section->listId != listId ||
            ((!listId || !listName.isUndefined()) && section->list != listName.toString()) ||
            (!sectionName.isUndefined() && section->section != sectionName.toString()))
            return fail("A reminder in the backup names the wrong section.");
    }
    const int skipped = int(imported.removeIf([&](const Imported &row) {
        return !row.reminder.hasContent() && !neededParents.contains(row.oldId);
    }));
    for (const Imported &row : std::as_const(imported))
        incomingNames.insert(row.entry.value("list").toString());

    Store store;
    if (!ready(store))
        return 1;
    Failures failures(store);
    // One step for everything, committed before the count is told.
    Store::Tx tx(&store, {}, Store::Latest);
    if (!tx)
        return failures.report("Nothing was imported");
    // What was here before the import. An export from Nudge 0.1.4 or later
    // gives each list and section an id, and two it holds apart stay apart
    // here, whatever they are called: each is matched to one that was here
    // already only by its name, and only once. Lists and sections the import
    // makes are only ever matched by their exact name, in an export without
    // ids, where the name is all there is.
    QSet<qint64> listsBefore, sectionsBefore;
    for (const TaskList &list : store.lists())
        listsBefore.insert(list.id);
    for (const Section &section : store.sections())
        sectionsBefore.insert(section.id);
    QHash<qint64, qint64> listIds, sectionIds;
    QHash<QString, qint64> legacyLists;
    QSet<qint64> claimed, claimedSections;
    // The lists the import makes, the only ones it may give the file's color
    // and order: nothing already here is changed.
    QSet<qint64> made;
    auto listFor = [&](qint64 oldId, const QString &name) -> qint64 {
        if (oldId && listIds.contains(oldId))
            return listIds.value(oldId);
        if (!oldId && legacyLists.contains(name))
            return legacyLists.value(name);
        if (name.isEmpty() && !oldId)
            return store.defaultListId();
        qint64 id = 0;
        for (const Qt::CaseSensitivity sensitivity : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
            for (const TaskList &list : store.lists()) {
                const bool before = listsBefore.contains(list.id);
                if (!before || claimed.contains(list.id))
                    continue;
                // The first spelling in an export must not take a list
                // another spelling would match exactly.
                if (sensitivity == Qt::CaseInsensitive && incomingNames.contains(list.name))
                    continue;
                if (list.name.compare(name, sensitivity) == 0) {
                    id = list.id;
                    break;
                }
            }
            if (id)
                break;
        }
        if (!id) {
            TaskList created;
            created.name = name;
            created.color = "blue";
            created.position = store.lists().isEmpty() ? 1 : store.lists().last().position + 1;
            id = store.put(created);
            made.insert(id);
        }
        if (oldId)
            listIds.insert(oldId, id);
        else
            legacyLists.insert(name, id);
        claimed.insert(id);
        return id;
    };
    auto sectionIn = [&](qint64 listId, qint64 oldId, const QString &name) -> qint64 {
        if (oldId && sectionIds.contains(oldId))
            return sectionIds.value(oldId);
        if (name.isEmpty())
            return 0;
        qint64 id = 0;
        for (const Section &section : store.sections()) {
            if (section.listId != listId || section.name != name)
                continue;
            if (oldId && (!sectionsBefore.contains(section.id) || claimedSections.contains(section.id)))
                continue;
            id = section.id;
            break;
        }
        if (!id) {
            Section section;
            section.listId = listId;
            section.name = name;
            section.position = store.sections().isEmpty() ? 1 : store.sections().last().position + 1;
            id = store.put(section);
        }
        if (oldId) {
            sectionIds.insert(oldId, id);
            claimedSections.insert(id);
        }
        return id;
    };
    for (const QJsonValue &value : document.value("lists").toArray()) {
        const QJsonObject entry = value.toObject();
        const qint64 listId = listFor(entry.value("id").toInteger(), entry.value("name").toString());
        if (const TaskList *list = store.list(listId); list && made.contains(listId)) {
            TaskList updated = *list;
            if (!entry.value("color").toString().isEmpty())
                updated.color = entry.value("color").toString();
            if (!entry.value("sort").toString().isEmpty())
                updated.sort = entry.value("sort").toString();
            store.put(updated);
        }
        const QJsonArray sections = entry.value("sections").toArray(), ids = entry.value("sectionIds").toArray();
        for (qsizetype i = 0; i < sections.size(); ++i)
            sectionIn(listId, i < ids.size() ? ids.at(i).toInteger() : 0, sections.at(i).toString());
    }

    // Parents first, so subtasks can find the ids their parents were given.
    QHash<qint64, qint64> newIds;
    QHash<qint64, double> positions;
    for (const Reminder &r : store.reminders())
        positions[r.listId] = qMax(positions.value(r.listId), r.position);
    int added = 0;
    for (const bool subtasks : {false, true}) {
        for (const auto &[entry, reminder, oldId, oldParent] : std::as_const(imported)) {
            if ((oldParent != 0) != subtasks)
                continue;
            Reminder r = reminder;
            r.title = reminderTitle(r);
            if (oldParent) {
                r.parentId = newIds.value(oldParent);
                const Reminder *parent = store.reminder(r.parentId);
                if (!parent)
                    continue; // the failed parent write will roll back the import
                r.listId = parent->listId;
                r.sectionId = parent->sectionId;
            } else {
                r.listId = listFor(entry.value("listId").toInteger(), entry.value("list").toString());
                r.sectionId = sectionIn(r.listId, entry.value("sectionId").toInteger(), entry.value("section").toString());
            }
            r.position = ++positions[r.listId];
            // A done reminder has no open subtasks, so an open one reopens
            // the parent it came with.
            if (const Reminder *parent = store.reminder(r.parentId); !r.completed && parent && parent->completed)
                store.reopen(r.parentId);
            const qint64 id = store.importReminder(r);
            if (oldId)
                newIds.insert(oldId, id);
            added += id != 0;
        }
    }
    if (!tx.commit() || failures.any())
        return failures.report("Nothing was imported");
    if (command.wantsJson()) {
        QJsonObject result{{"imported", added}};
        if (skipped)
            result["skipped"] = skipped;
        print(result);
    } else {
        print(stdout, QString("Imported %1 reminders").arg(added));
        if (skipped)
            print(stdout, QString("Skipped %1 empty %2").arg(skipped).arg(skipped == 1 ? "reminder" : "reminders"));
    }
    return 0;
}

int background(const QStringList &arguments, const QString &unitFile) {
    Command command("background", "Whether reminders arrive while the window is closed: background on, background off.");
    command.parser.addPositionalArgument("state", "on or off", "[on|off]");
    command.json();
    command.parser.process(arguments);
    const QString state = command.arguments().value(0);
    if (!state.isEmpty() && state != "on" && state != "off")
        return fail("Use nudge background, nudge background on, or nudge background off.");
    if (!QFile::exists(unitFile))
        return fail("Reminders in the background need Nudge installed as a package (bin/install).");
    QString output, error;
    auto systemctl = [&](const QStringList &more) {
        QProcess process;
        process.start("systemctl", QStringList{"--user"} + more);
        if (!process.waitForFinished(8000) || process.exitStatus() != QProcess::NormalExit) {
            error = process.errorString();
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished();
            }
            return -1;
        }
        output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (error.isEmpty())
            error = output.isEmpty() ? QString("systemctl exited with status %1").arg(process.exitCode()) : output;
        return process.exitCode();
    };
    if (!state.isEmpty() && systemctl({state == "on" ? "enable" : "disable", "--now", "nudge.service"}) != 0)
        return fail("systemctl could not turn background reminders " + state + ": " + error);
    const int code = systemctl({"is-enabled", "nudge.service"});
    const auto on = backgroundEnabled(output, code);
    if (!on.has_value())
        return fail(QString("Could not check background reminders: %1").arg(error));
    if (command.wantsJson())
        print({{"background", *on}});
    else
        print(stdout, *on ? "Reminders arrive even when the window is closed."
                         : "Reminders only arrive while the window is open.");
    return 0;
}

// One shared copy of the skill, also linked into the older skills directory.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/nudge and link into ~/.claude/skills",
                                         "[install]");
    command.json();
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return fail("The skill is missing from this build.");
    const QByteArray text = bundled.readAll();
    const QString action = command.arguments().value(0);
    if (action.isEmpty()) {
        if (command.wantsJson())
            print({{"skill", QString::fromUtf8(text)}});
        else
            fputs(text.constData(), stdout);
        return 0;
    }
    if (action != "install")
        return fail("Use nudge skill to print the skill, or nudge skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/nudge");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) != text.size())
        return fail("Could not write " + file.fileName());

    const QString link = home.exists(".claude") ? home.filePath(".claude/skills/nudge") : QString();
    bool linkedNow = false;
    QString previous;
    std::optional<QTemporaryDir> holding;
    auto restore = qScopeGuard([&] {
        if (linkedNow)
            QFile::remove(link);
        if (!previous.isEmpty())
            QFile::rename(previous, link);
    });
    if (!link.isEmpty()) {
        if (!home.mkpath(".claude/skills"))
            return fail("Could not link " + link);
        const QFileInfo existing(link);
        const QString canonical = QFileInfo(directory).canonicalFilePath();
        const QDir parent(existing.dir().canonicalPath());
        if (existing.canonicalFilePath() != canonical) {
            if (existing.isSymLink()) {
                holding.emplace(parent.filePath(".nudge-skill.XXXXXX"));
                if (!holding->isValid() || !QFile::rename(link, holding->filePath("previous")))
                    return fail("Could not replace the link " + link);
                previous = holding->filePath("previous");
            } else if (existing.exists()) {
                return fail(link + " exists and is not a link; leaving it alone.");
            }
            const QString target = parent.relativeFilePath(canonical);
            if (!QFile::link(target, link))
                return fail("Could not link " + link);
            linkedNow = true;
        }
    }
    if (!file.commit())
        return fail("Could not write " + file.fileName());
    restore.dismiss();
    if (command.wantsJson()) {
        print({{"installed", file.fileName()}, {"linked", link.isEmpty() ? QJsonValue() : QJsonValue(link)}});
    } else {
        print(stdout, "Installed " + file.fileName());
        if (!link.isEmpty())
            print(stdout, "Linked " + link);
    }
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: nudge <command> [options]\n\n" + cliSummary());
    return 0;
}

} // namespace

QString cliSummary() {
    return "Window:\n"
           "  open                     Open the window\n\n"
           "Commands that need no display (nudge help <command> for options):\n"
           "  add <text>               Add a reminder: nudge add \"Call the dentist tomorrow at 9 #health\"\n"
           "  list [view]              List reminders: today, scheduled, all, flagged, completed\n"
           "  show <id>                Show one reminder and its subtasks\n"
           "  edit <id>                Change a reminder\n"
           "  done <id>...             Complete reminders\n"
           "  reopen <id>...           Mark reminders as not done\n"
           "  delete <id>...           Delete reminders\n"
           "  lists                    Show the lists; lists add <name>, lists delete <name>\n"
           "  export [file]            Write everything as JSON\n"
           "  import <file>            Add the lists and reminders of an export\n"
           "  background [on|off]      Reminders while the window is closed\n"
           "  skill [install]          Print the skill for coding agents, or install it\n"
           "  daemon                   Send the alerts; the background service runs this";
}

std::optional<bool> backgroundEnabled(const QString &output, int exitCode) {
    if (exitCode == 0 && (output == "enabled" || output == "enabled-runtime"))
        return true;
    static const QStringList off{"disabled", "masked", "masked-runtime", "linked", "linked-runtime",
                                 "static", "indirect", "alias", "generated", "transient"};
    if ((exitCode == 0 || exitCode == 1) && off.contains(output))
        return false;
    return std::nullopt;
}

bool isCliCommand(const QString &word) {
    return QStringList{"add", "list", "show", "edit", "done", "reopen", "delete", "lists", "export",
                       "import", "background", "skill", "help"}
        .contains(word);
}

int runCli(const QStringList &arguments, const QString &unitFile) {
    const QString command = arguments.value(1);
    if (command == "add")
        return add(arguments);
    if (command == "list")
        return list(arguments);
    if (command == "show")
        return show(arguments);
    if (command == "edit")
        return edit(arguments);
    if (command == "done")
        return each(arguments, "done", "Complete reminders. A repeating one moves to its next date.", "Completed");
    if (command == "reopen")
        return each(arguments, "reopen", "Mark reminders as not done.", "Reopened");
    if (command == "delete")
        return each(arguments, "delete", "Delete reminders, with their subtasks.", "Deleted");
    if (command == "lists")
        return lists(arguments);
    if (command == "export")
        return exportAll(arguments);
    if (command == "import")
        return importAll(arguments);
    if (command == "background")
        return background(arguments, unitFile);
    if (command == "skill")
        return skill(arguments);
    return help(arguments);
}

WindowLaunch windowLaunch(const QString &nudge, const QProcessEnvironment &session, const QString &systemdRun,
                          qint64 reminderId) {
    QStringList arguments{"open"};
    if (reminderId > 0)
        arguments << "--reminder" << QString::number(reminderId);
    if (systemdRun.isEmpty())
        return {nudge, arguments, session};
    return {systemdRun, QStringList{"--user", "--scope", "--collect", "--quiet", nudge} + arguments, session};
}

int runCliInDesktopLocale(const QStringList &arguments, const QString &languageFile) {
    Translator translator(languageFile);
    QCoreApplication::installTranslator(&translator);
    const int result = runCli(arguments);
    QCoreApplication::removeTranslator(&translator);
    return result;
}

int runDaemon(const QProcessEnvironment &session) {
    Store store;
    if (!ready(store))
        return 1;
    store.watch();
    DesktopAlerts alerts(true);
    if (!alerts.problem().isEmpty())
        return fail(alerts.problem());
    Notifier notifier(&store, &alerts);
    QObject::connect(&alerts, &DesktopAlerts::acted, &notifier, [&notifier](quint32 id, const QString &action) {
        notifier.act(id, action, QDateTime::currentDateTime());
    });
    // Clicking an alert brings up the window: the one already open, or a new one.
    QObject::connect(&notifier, &Notifier::opened, &notifier, [&store, session](qint64 id) {
        if (openExistingNudgeWindow(store.path(), id))
            return;
        const WindowLaunch launch = windowLaunch(QCoreApplication::applicationFilePath(), session,
                                                 QStandardPaths::findExecutable("systemd-run"), id);
        QProcess process;
        process.setProgram(launch.program);
        process.setArguments(launch.arguments);
        process.setProcessEnvironment(launch.environment);
        process.startDetached();
    });
    notifier.start();
    return QCoreApplication::exec();
}

namespace {

// A signal handler may do next to nothing, so it only writes a byte for the
// event loop to read.
int stopSignal[2] = {-1, -1};

void onStop(int) {
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(stopSignal[1], &byte, 1);
}

} // namespace

// systemd stops the background service, and logging out stops the window,
// with SIGTERM. Quitting through the event loop closes the window as a click
// would, keeping a word being typed, and lets the Notifier take the buttons
// off alerts that no process will answer any more.
void quitOnStop(QCoreApplication &app) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stopSignal) != 0)
        return;
    auto *stop = new QSocketNotifier(stopSignal[0], QSocketNotifier::Read, &app);
    QObject::connect(stop, &QSocketNotifier::activated, &app, [] {
        char byte;
        [[maybe_unused]] const ssize_t drained = ::read(stopSignal[0], &byte, 1);
        QCoreApplication::quit();
    });
    struct sigaction action = {};
    action.sa_handler = onStop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        struct sigaction old;
        // One it was started ignoring stays ignored: nohup and a script's background jobs ask for that.
        if (sigaction(signal, nullptr, &old) == 0 && old.sa_handler == SIG_IGN)
            continue;
        sigaction(signal, &action, nullptr);
    }
}
