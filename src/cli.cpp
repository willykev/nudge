// The command layout follows Hype's src/cli.cpp (MIT, David Heinemeier Hansson).
#include "cli.h"

#include "alerts.h"
#include "dates.h"
#include "recurrence.h"
#include "remindermodel.h"
#include "store.h"

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
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <functional>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const QStringList priorities{"none", "low", "medium", "high"};

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

const TaskList *listNamed(const Store &store, const QString &name) {
    for (const TaskList &list : store.lists())
        if (list.name.compare(name, Qt::CaseInsensitive) == 0)
            return &list;
    return nullptr;
}

QString listNames(const Store &store) {
    QStringList names;
    for (const TaskList &list : store.lists())
        names << oneLine(list.name);
    return names.isEmpty() ? QString("There are no lists yet.") : "Lists: " + names.join(", ");
}

QString isoTime(int minutes) {
    return QString("%1:%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0'));
}

QJsonValue isoMoment(qint64 seconds) {
    return seconds ? QJsonValue(QDateTime::fromSecsSinceEpoch(seconds).toString(Qt::ISODate)) : QJsonValue();
}

QJsonObject toJson(const Store &store, const Reminder &r) {
    const TaskList *list = store.list(r.listId);
    const Section *section = store.section(r.sectionId);
    return {{"id", r.id},
            {"title", r.title},
            {"notes", r.notes},
            {"url", r.url},
            {"list", list ? list->name : QString()},
            {"section", section ? QJsonValue(section->name) : QJsonValue()},
            {"parent", r.parentId ? QJsonValue(r.parentId) : QJsonValue()},
            {"due", r.due.isValid() ? QJsonValue(r.due.toString(Qt::ISODate)) : QJsonValue()},
            {"time", r.dueMinutes >= 0 ? QJsonValue(isoTime(r.dueMinutes)) : QJsonValue()},
            {"repeat", r.repeat.isEmpty() ? QJsonValue() : QJsonValue(r.repeat)},
            {"repeatEnd", r.repeatEnd.isValid() ? QJsonValue(r.repeatEnd.toString(Qt::ISODate)) : QJsonValue()},
            {"early", r.early},
            {"priority", priorities.value(r.priority, "none")},
            {"flagged", r.flagged},
            {"tags", QJsonArray::fromStringList(r.tags)},
            {"completed", r.completed},
            {"completedAt", isoMoment(r.completedAt)},
            {"createdAt", isoMoment(r.createdAt)}};
}

QString toText(const Store &store, const Reminder &r) {
    QStringList parts{QString("%1").arg(r.id, 5), r.completed ? "[x]" : "[ ]",
                      QString(r.parentId ? "  " : "") + r.title};
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

// A monthly or yearly reminder keeps returning to the day it is set for.
void anchor(Reminder &r) {
    Repeat repeat = Repeat::parse(r.repeat);
    if (repeat.unit != Repeat::Monthly && repeat.unit != Repeat::Yearly)
        return;
    repeat.anchorDay = r.due.day();
    r.repeat = repeat.toString();
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
        repeat = Repeat::parse(entry.repeat);
        if (entry.due.isValid())
            date = entry.due;
    }
    if (!repeat.repeats())
        return false;
    if (repeat.unit == Repeat::Weekdays)
        while (date.dayOfWeek() > 5)
            date = date.addDays(1);
    r.due = date;
    r.repeat = repeat.toString();
    anchor(r);
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
        const TaskList *list = listNamed(store, parser.value("list"));
        if (!list)
            return QString("No list named %1. %2").arg(parser.value("list"), listNames(store));
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
            r.dueMinutes = -1;
            r.repeat.clear();
            r.repeatEnd = QDate();
            r.early = 0;
        } else {
            const QDate date = Dates::parseDate(text, today);
            if (!date.isValid())
                return QString("Could not read %1 as a date. Try 2026-12-24, tomorrow, or \"next friday\".").arg(text);
            r.due = date;
            anchor(r);
        }
    }
    if (parser.isSet("time")) {
        const QString text = parser.value("time");
        if (text == "none") {
            r.dueMinutes = -1;
        } else {
            const int minutes = Dates::parseTime(text);
            if (minutes < 0)
                return QString("Could not read %1 as a time. Try 17:30 or 5pm.").arg(text);
            r.dueMinutes = minutes;
            // A time needs a day: the next one the clock shows that time on.
            if (!r.due.isValid())
                r.due = minutes > now.time().hour() * 60 + now.time().minute() ? today : today.addDays(1);
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
    const QString text = command.arguments().join(' ').simplified();
    if (text.isEmpty())
        return fail("Say what to be reminded of: nudge add \"Water the plants every friday\"");

    Store store;
    if (!ready(store))
        return 1;
    Reminder r;
    QuickEntry entry;
    entry.title = text;
    if (!command.parser.isSet("literal"))
        entry = Dates::parseQuick(text, QDateTime::currentDateTime());
    r.title = entry.title;
    r.due = entry.due;
    r.dueMinutes = entry.minutes;
    r.repeat = entry.repeat;
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
    if (parentId && r.listId != listId)
        return fail("A subtask lives in its parent's list; leave out --list.");

    // A done reminder has no open subtasks, so adding one reopens it.
    const Reminder *parent = store.reminder(parentId);
    const bool reopen = parent && parent->completed;
    Failures failures(store);
    qint64 id = 0;
    {
        Store::Tx tx(&store);
        if (reopen)
            store.reopen(parentId);
        id = store.put(r);
    }
    if (failures.any())
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

    Failures failures(store);
    {
        Store::Tx tx(&store);
        if (r.listId != listId) {
            // Its subtasks move with it.
            for (qint64 childId : store.childrenOf(id)) {
                Reminder child = *store.reminder(childId);
                child.listId = r.listId;
                child.sectionId = 0;
                store.put(child);
            }
        }
        store.put(r);
    }
    if (failures.any())
        return failures.report("The change could not be saved");
    return report(command, store, id, "Changed");
}

// done, reopen, and delete all take one or more ids, and do nothing unless
// every one of them exists. An id named twice is acted on once: done twice
// would skip a repeating reminder's next date.
int each(const QStringList &arguments, const QString &name, const QString &description, const QString &verb,
         const std::function<void(Store &, qint64)> &action) {
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
    // Titles are read before anything is done: deleting a parent takes its
    // subtasks with it, so one listed after it is gone by its turn.
    QHash<qint64, QString> titles;
    for (const QString &word : command.arguments()) {
        const qint64 id = word.toLongLong();
        const Reminder *r = store.reminder(id);
        if (!r)
            return fail("There is no reminder " + word + ". Nothing was changed.");
        if (!ids.contains(id)) {
            ids << id;
            titles.insert(id, r->title);
        }
    }
    QJsonArray changed;
    QStringList lines;
    Failures failures(store);
    {
        Store::Tx tx(&store);
        for (qint64 id : ids) {
            const QString title = titles.value(id);
            lines << QString("%1 %2: %3").arg(verb, QString::number(id), oneLine(title));
            action(store, id);
            if (const Reminder *r = store.reminder(id))
                changed << toJson(store, *r);
            else
                changed << QJsonObject{{"id", id}, {"title", title}, {"deleted", true}};
        }
    }
    if (failures.any())
        return failures.report("Nothing was changed");
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
    const QString name = command.arguments().value(0, "all");
    View view = View::fromKey(name);
    if (view.key() != name)
        return fail("Views are: today, scheduled, all, flagged, completed.");
    if (command.parser.isSet("list")) {
        const TaskList *found = listNamed(store, command.parser.value("list"));
        if (!found)
            return fail(QString("No list named %1. %2").arg(command.parser.value("list"), listNames(store)));
        view = View{View::List, found->id, {}};
    } else if (command.parser.isSet("tag")) {
        view = View{View::Tag, 0, QString(command.parser.value("tag")).remove('#')};
    } else if (command.parser.isSet("search")) {
        view = View{View::Search, 0, command.parser.value("search").simplified()};
    }

    ViewOptions options;
    options.now = QDateTime::currentDateTime();
    QJsonArray found;
    for (const Row &row : buildRows(store, view, options)) {
        const Reminder *r = row.heading ? nullptr : store.reminder(row.id);
        // One without a title is being typed in the window right now.
        if (!r || r->title.isEmpty())
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
    const QString name = command.arguments().mid(1).join(' ').simplified();
    auto count = [&](qint64 listId, bool openOnly) {
        int total = 0;
        for (const Reminder &r : store.reminders())
            total += r.listId == listId && !(openOnly && r.completed);
        return total;
    };

    if (action == "add") {
        if (name.isEmpty())
            return fail("Name the list: nudge lists add Groceries");
        if (listNamed(store, name))
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
        Failures failures(store);
        created.id = store.put(created);
        if (failures.any() || !created.id)
            return failures.report("The list could not be saved");
        if (command.wantsJson())
            print({{"id", created.id}, {"name", created.name}, {"color", created.color}});
        else
            print(stdout, "Added the list " + created.name);
        return 0;
    }
    if (action == "delete") {
        const TaskList *doomed = listNamed(store, name);
        if (!doomed)
            return fail(QString("No list named %1. %2").arg(name, listNames(store)));
        const int holding = count(doomed->id, false);
        if (holding && !command.parser.isSet("force"))
            return fail(QString("%1 holds %2 reminders. Add --force to delete them with it.")
                            .arg(oneLine(doomed->name), QString::number(holding)));
        const QString removed = doomed->name;
        Failures failures(store);
        store.removeList(doomed->id);
        if (failures.any())
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
    QJsonArray lists, reminders;
    for (const TaskList &entry : store.lists()) {
        QJsonArray sections;
        for (const Section &section : store.sections())
            if (section.listId == entry.id)
                sections << section.name;
        lists << QJsonObject{{"name", entry.name}, {"color", entry.color}, {"sort", entry.sort}, {"sections", sections}};
    }
    QVector<const Reminder *> ordered;
    for (const Reminder &r : store.reminders())
        ordered << &r;
    std::sort(ordered.begin(), ordered.end(), [](const Reminder *a, const Reminder *b) {
        return a->listId != b->listId ? a->listId < b->listId
               : a->position != b->position ? a->position < b->position
                                            : a->id < b->id;
    });
    for (const Reminder *r : ordered)
        reminders << toJson(store, *r);
    const QJsonObject document{{"nudge", 1},
                               {"exportedAt", QDateTime::currentDateTime().toString(Qt::ISODate)},
                               {"lists", lists},
                               {"reminders", reminders}};
    const QString path = command.arguments().value(0);
    // To standard output it is JSON, whether --json says so or not.
    if (path.isEmpty()) {
        print(document);
        return 0;
    }
    // Written beside the target and renamed over it, so a failure halfway
    // leaves any earlier export whole.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(document).toJson()) < 0 || !file.commit())
        return fail("Could not write " + path + ": " + file.errorString());
    if (command.wantsJson())
        print({{"exported", int(reminders.size())}, {"file", QFileInfo(path).absoluteFilePath()}});
    else
        print(stdout, QString("Exported %1 reminders to %2").arg(reminders.size()).arg(path));
    return 0;
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
    if (problem.error != QJsonParseError::NoError || !document.contains("reminders"))
        return fail(path + " is not a Nudge export.");

    Store store;
    if (!ready(store))
        return 1;
    Failures failures(store);
    // One step for everything, committed before the count is told.
    std::optional<Store::Tx> tx(std::in_place, &store);
    auto sectionIn = [&](qint64 listId, const QString &name) -> qint64 {
        if (name.isEmpty())
            return 0;
        for (const Section &section : store.sections())
            if (section.listId == listId && section.name == name)
                return section.id;
        Section section;
        section.listId = listId;
        section.name = name;
        section.position = store.sections().isEmpty() ? 1 : store.sections().last().position + 1;
        return store.put(section);
    };
    // The lists the import makes, the only ones it may give the file's color
    // and order: nothing already here is changed.
    QSet<qint64> made;
    auto listFor = [&](const QString &name) -> qint64 {
        if (name.isEmpty())
            return store.defaultListId();
        if (const TaskList *found = listNamed(store, name))
            return found->id;
        TaskList created;
        created.name = name;
        created.color = "blue";
        created.position = store.lists().isEmpty() ? 1 : store.lists().last().position + 1;
        const qint64 id = store.put(created);
        made.insert(id);
        return id;
    };
    for (const QJsonValue &value : document.value("lists").toArray()) {
        const QJsonObject entry = value.toObject();
        const qint64 listId = listFor(entry.value("name").toString());
        if (const TaskList *list = store.list(listId); list && made.contains(listId)) {
            TaskList updated = *list;
            if (!entry.value("color").toString().isEmpty())
                updated.color = entry.value("color").toString();
            if (!entry.value("sort").toString().isEmpty())
                updated.sort = entry.value("sort").toString();
            store.put(updated);
        }
        for (const QJsonValue &section : entry.value("sections").toArray())
            sectionIn(listId, section.toString());
    }

    // Parents first, so subtasks can find the ids their parents were given.
    QHash<qint64, qint64> newIds;
    int added = 0;
    for (const bool subtasks : {false, true}) {
        for (const QJsonValue &value : document.value("reminders").toArray()) {
            const QJsonObject entry = value.toObject();
            const qint64 oldParent = entry.value("parent").toInteger();
            if ((oldParent != 0) != subtasks)
                continue;
            Reminder r;
            r.listId = listFor(entry.value("list").toString());
            r.sectionId = sectionIn(r.listId, entry.value("section").toString());
            r.parentId = newIds.value(oldParent);
            r.title = entry.value("title").toString();
            r.notes = entry.value("notes").toString();
            r.url = entry.value("url").toString();
            r.due = QDate::fromString(entry.value("due").toString(), Qt::ISODate);
            const QTime time = QTime::fromString(entry.value("time").toString(), "HH:mm");
            r.dueMinutes = r.due.isValid() && time.isValid() ? time.hour() * 60 + time.minute() : -1;
            r.repeat = Repeat::parse(entry.value("repeat").toString()).toString();
            r.repeatEnd = QDate::fromString(entry.value("repeatEnd").toString(), Qt::ISODate);
            r.early = entry.value("early").toInt();
            r.priority = qMax(0, int(priorities.indexOf(entry.value("priority").toString())));
            r.flagged = entry.value("flagged").toBool();
            for (const QJsonValue &tag : entry.value("tags").toArray())
                r.tags << tag.toString();
            r.completed = entry.value("completed").toBool();
            r.completedAt = QDateTime::fromString(entry.value("completedAt").toString(), Qt::ISODate).toSecsSinceEpoch();
            r.createdAt = QDateTime::fromString(entry.value("createdAt").toString(), Qt::ISODate).toSecsSinceEpoch();
            if (r.completedAt < 0)
                r.completedAt = 0;
            if (r.createdAt < 0)
                r.createdAt = 0;
            r.position = store.endPosition(r.listId);
            // A done reminder has no open subtasks, so an open one reopens
            // the parent it came with.
            if (const Reminder *parent = store.reminder(r.parentId); !r.completed && parent && parent->completed)
                store.reopen(r.parentId);
            const qint64 id = store.put(r);
            newIds.insert(entry.value("id").toInteger(), id);
            added += id != 0;
        }
    }
    tx.reset();
    if (failures.any())
        return failures.report("Nothing was imported");
    if (command.wantsJson())
        print({{"imported", added}});
    else
        print(stdout, QString("Imported %1 reminders").arg(added));
    return 0;
}

int background(const QStringList &arguments) {
    Command command("background", "Whether reminders arrive while the window is closed: background on, background off.");
    command.parser.addPositionalArgument("state", "on or off", "[on|off]");
    command.json();
    command.parser.process(arguments);
    const QString state = command.arguments().value(0);
    if (!state.isEmpty() && state != "on" && state != "off")
        return fail("Use nudge background, nudge background on, or nudge background off.");
    if (!QFile::exists("/usr/lib/systemd/user/nudge.service"))
        return fail("Reminders in the background need Nudge installed as a package (bin/install).");
    auto systemctl = [](const QStringList &more, QString *output) {
        QProcess process;
        process.start("systemctl", QStringList{"--user"} + more);
        if (!process.waitForFinished(8000))
            return -1;
        *output = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError()).trimmed();
        return process.exitCode();
    };
    QString output;
    if (!state.isEmpty() && systemctl({state == "on" ? "enable" : "disable", "--now", "nudge.service"}, &output) != 0)
        return fail("systemctl could not turn background reminders " + state + ": " + output);
    systemctl({"is-enabled", "nudge.service"}, &output);
    const bool on = output == "enabled";
    if (command.wantsJson())
        print({{"background", on}});
    else
        print(stdout, on ? "Reminders arrive even when the window is closed."
                         : "Reminders only arrive while the window is open.");
    return 0;
}

// As Hype does: one shared copy, linked into Claude Code, found directly by Codex.
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
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    if (!command.wantsJson())
        print(stdout, "Installed " + file.fileName());
    QJsonValue linked; // stays null without Claude Code
    if (home.exists(".claude")) {
        const QString link = home.filePath(".claude/skills/nudge");
        const QFileInfo existing(link);
        if (existing.isSymLink())
            QFile::remove(link);
        else if (existing.exists())
            return fail(link + " exists and is not a link; leaving it alone.");
        if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/nudge", link))
            return fail("Could not link " + link);
        if (!command.wantsJson())
            print(stdout, "Linked " + link);
        linked = link;
    }
    if (command.wantsJson())
        print({{"installed", file.fileName()}, {"linked", linked}});
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

bool isCliCommand(const QString &word) {
    return QStringList{"add", "list", "show", "edit", "done", "reopen", "delete", "lists", "export",
                       "import", "background", "skill", "help"}
        .contains(word);
}

int runCli(const QStringList &arguments) {
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
        return each(arguments, "done", "Complete reminders. A repeating one moves to its next date.", "Completed",
                    [](Store &store, qint64 id) { store.complete(id, QDateTime::currentDateTime()); });
    if (command == "reopen")
        return each(arguments, "reopen", "Mark reminders as not done.", "Reopened",
                    [](Store &store, qint64 id) { store.reopen(id); });
    if (command == "delete")
        return each(arguments, "delete", "Delete reminders, with their subtasks.", "Deleted",
                    [](Store &store, qint64 id) { store.removeReminder(id); });
    if (command == "lists")
        return lists(arguments);
    if (command == "export")
        return exportAll(arguments);
    if (command == "import")
        return importAll(arguments);
    if (command == "background")
        return background(arguments);
    if (command == "skill")
        return skill(arguments);
    return help(arguments);
}

WindowLaunch windowLaunch(const QString &nudge, const QProcessEnvironment &session, const QString &systemdRun) {
    if (systemdRun.isEmpty())
        return {nudge, {"open"}, session};
    return {systemdRun, {"--user", "--scope", "--collect", "--quiet", nudge, "open"}, session};
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
    QObject::connect(&notifier, &Notifier::opened, &notifier, [session] {
        if (focusNudgeWindow())
            return;
        const WindowLaunch launch = windowLaunch(QCoreApplication::applicationFilePath(), session,
                                                 QStandardPaths::findExecutable("systemd-run"));
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
