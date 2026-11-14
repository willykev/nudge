#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

#include "alerts.h"
#include "backend.h"
#include "cli.h"
#include "dates.h"
#include "look.h"
#include "recurrence.h"
#include "remindermodel.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

namespace {

// Every test runs on the same Friday morning.
const QDateTime friday(QDate(2026, 11, 13), QTime(10, 0));

struct Sent {
    QString title, body;
    QStringList actions;
};

// Records alerts instead of showing them.
class FakeAlerts : public Alerts {
public:
    quint32 show(const QString &title, const QString &body, const QStringList &actions) override {
        sent.append({title, body, actions});
        return quint32(sent.size());
    }
    void disarm(quint32 id, const QString &title, const QString &body) override {
        disarmed.insert(id, {title, body, {}});
    }
    bool bodyMarkup() override { return markup; }
    bool mayAlert() override { return allowed; }

    QVector<Sent> sent;
    QMap<quint32, Sent> disarmed; // shown again in place, without buttons
    bool allowed = true;
    bool markup = false; // whether the desktop reads the body as markup
};

qint64 addList(Store &store, const QString &name) {
    TaskList list;
    list.name = name;
    list.color = "#60A5FA";
    list.position = store.lists().size() + 1;
    return store.put(list);
}

qint64 add(Store &store, qint64 listId, const QString &title, const QDate &due = {}, int minutes = -1) {
    Reminder r;
    r.listId = listId;
    r.title = title;
    r.due = due;
    r.dueMinutes = minutes;
    r.position = store.endPosition(listId);
    return store.put(r);
}

QStringList titles(const Store &store, const QVector<Row> &rows) {
    QStringList result;
    for (const Row &row : rows)
        result << (row.heading ? "[" + row.text + "]" : QString(row.depth * 2, ' ') + store.reminder(row.id)->title);
    return result;
}

} // namespace

class NudgeTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Basic");
    }
    void init() { QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); }

    // ---- Repeats ---------------------------------------------------------------

    void repeatsStepToTheNextDate() {
        const QDate day(2026, 11, 13);
        QCOMPARE(Repeat::parse("daily:1").after(day), QDate(2026, 11, 14));
        QCOMPARE(Repeat::parse("daily:3").after(day), QDate(2026, 11, 16));
        QCOMPARE(Repeat::parse("weekly:2").after(day), QDate(2026, 11, 27));
        QCOMPARE(Repeat::parse("weekdays").after(day), QDate(2026, 11, 16)); // Friday to Monday
        QCOMPARE(Repeat::parse("yearly:1").after(day), QDate(2027, 11, 13));
        QVERIFY(!Repeat::parse("").repeats());
        QVERIFY(!Repeat::parse("nonsense:4").repeats());
        QCOMPARE(Repeat::parse("weekly:2").toString(), QString("weekly:2"));
    }

    void monthlyRepeatsReturnToTheirDay() {
        const Repeat monthly = Repeat::parse("monthly:1:31");
        const QDate february = monthly.after(QDate(2027, 1, 31));
        QCOMPARE(february, QDate(2027, 2, 28));
        QCOMPARE(monthly.after(february), QDate(2027, 3, 31));
        QCOMPARE(Repeat::parse("yearly:1:29").after(QDate(2028, 2, 29)), QDate(2029, 2, 28));
    }

    void overdueRepeatsSkipToTheNextOneAhead() {
        const Repeat daily = Repeat::parse("daily:1");
        QCOMPARE(daily.upcoming(QDate(2026, 10, 1), friday.date()), friday.date());
        QVERIFY(!daily.upcoming(QDate(2026, 10, 1), friday.date(), QDate(2026, 11, 10)).isValid());
    }

    // ---- Reading dates out of text ------------------------------------------------

    void quickEntryReadsDatesTagsAndPriority() {
        const QuickEntry entry = Dates::parseQuick("Call the dentist tomorrow at 9 #health !!", friday);
        QCOMPARE(entry.title, QString("Call the dentist"));
        QCOMPARE(entry.due, QDate(2026, 11, 14));
        QCOMPARE(entry.minutes, 9 * 60);
        QCOMPARE(entry.tags, QStringList{"health"});
        QCOMPARE(entry.priority, 2);
    }

    void quickEntryReadsRepeats() {
        QuickEntry entry = Dates::parseQuick("Water the plants every friday", friday);
        QCOMPARE(entry.title, QString("Water the plants"));
        QCOMPARE(entry.repeat, QString("weekly:1"));
        QCOMPARE(entry.due, friday.date());

        entry = Dates::parseQuick("Pay rent every month", friday);
        QCOMPARE(entry.repeat, QString("monthly:1:13"));

        entry = Dates::parseQuick("Standup every weekday at 9:30", friday);
        QCOMPARE(entry.repeat, QString("weekdays"));
        QCOMPARE(entry.due, QDate(2026, 11, 16)); // 9:30 has passed, and tomorrow is Saturday
        QCOMPARE(entry.minutes, 9 * 60 + 30);

        entry = Dates::parseQuick("Back up the laptop every other week", friday);
        QCOMPARE(entry.repeat, QString("weekly:2"));
        entry = Dates::parseQuick("Stretch every 2 days", friday);
        QCOMPARE(entry.repeat, QString("daily:2"));
    }

    void quickEntryReadsRelativeAndWrittenDates() {
        QCOMPARE(Dates::parseQuick("Meeting in 2 hours", friday).minutes, 12 * 60);
        QCOMPARE(Dates::parseQuick("Follow up in 3 days", friday).due, QDate(2026, 11, 16));
        QCOMPARE(Dates::parseQuick("Submit taxes apr 15", friday).due, QDate(2027, 4, 15));
        QCOMPARE(Dates::parseQuick("Party 5th december", friday).due, QDate(2026, 12, 5));
        QCOMPARE(Dates::parseQuick("Launch 2027-01-05", friday).due, QDate(2027, 1, 5));
        QCOMPARE(Dates::parseQuick("Review next friday", friday).due, QDate(2026, 11, 20));
        QCOMPARE(Dates::parseQuick("Clean up this weekend", friday).due, QDate(2026, 11, 14));
        QCOMPARE(Dates::parseQuick("Dinner 7pm", friday).minutes, 19 * 60);
        QCOMPARE(Dates::parseQuick("Dinner 7pm", friday).due, friday.date());
        QCOMPARE(Dates::parseQuick("Call at 5", friday).minutes, 17 * 60);
        QCOMPARE(Dates::parseQuick("Stand up 9am", friday).due, QDate(2026, 11, 14)); // already past today
        QCOMPARE(Dates::parseQuick("Movie tonight", friday).minutes, 20 * 60);
    }

    void quickEntryLeavesOrdinaryWordsAlone() {
        QuickEntry entry = Dates::parseQuick("Buy sun cream", friday);
        QCOMPARE(entry.title, QString("Buy sun cream"));
        QVERIFY(!entry.due.isValid());

        entry = Dates::parseQuick("Review 5.30 budget", friday);
        QCOMPARE(entry.title, QString("Review 5.30 budget"));
        QVERIFY(!entry.due.isValid());

        entry = Dates::parseQuick("Read chapter 5", friday);
        QCOMPARE(entry.title, QString("Read chapter 5"));
        QVERIFY(!entry.due.isValid());

        // A short weekday counts at the end of the line.
        entry = Dates::parseQuick("Lunch with Sam sun", friday);
        QCOMPARE(entry.title, QString("Lunch with Sam"));
        QCOMPARE(entry.due, QDate(2026, 11, 15));

        // A line that is only a date stays a title.
        entry = Dates::parseQuick("tomorrow", friday);
        QCOMPARE(entry.title, QString("tomorrow"));
        QVERIFY(!entry.due.isValid());
    }

    void dateAndTimeFieldsTakePhrases() {
        const QDate today = friday.date();
        QCOMPARE(Dates::parseDate("tomorrow", today), QDate(2026, 11, 14));
        QCOMPARE(Dates::parseDate("fri", today), QDate(2026, 11, 20));
        QCOMPARE(Dates::parseDate("next week", today), QDate(2026, 11, 16));
        QCOMPARE(Dates::parseDate("20", today), QDate(2026, 11, 20));
        QCOMPARE(Dates::parseDate("5", today), QDate(2026, 12, 5));
        QCOMPARE(Dates::parseDate("dec 24", today), QDate(2026, 12, 24));
        QCOMPARE(Dates::parseDate("12/24", today), QDate(2026, 12, 24));
        QCOMPARE(Dates::parseDate("2027-01-05", today), QDate(2027, 1, 5));
        QVERIFY(!Dates::parseDate("soonish", today).isValid());

        QCOMPARE(Dates::parseTime("5pm"), 17 * 60);
        QCOMPARE(Dates::parseTime("17:30"), 17 * 60 + 30);
        QCOMPARE(Dates::parseTime("9"), 9 * 60);
        QCOMPARE(Dates::parseTime("5"), 17 * 60);
        QCOMPARE(Dates::parseTime("noon"), 12 * 60);
        QCOMPARE(Dates::parseTime("1730"), 17 * 60 + 30);
        QCOMPARE(Dates::parseTime("later"), -1);
    }

    void datesReadTheWayTheLocaleWritesThem() {
        const QDate today = friday.date();
        QCOMPARE(Dates::formatDay(today, today), QString("Today"));
        QCOMPARE(Dates::formatDay(today.addDays(1), today), QString("Tomorrow"));
        QCOMPARE(Dates::formatDay(today.addDays(-1), today), QString("Yesterday"));
        QCOMPARE(Dates::formatDay(today.addDays(3), today), QString("Monday"));
        QCOMPARE(Dates::formatDay(QDate(2026, 12, 1), today), QString("Tue, Dec 1"));
        QCOMPARE(Dates::formatDay(QDate(2027, 1, 1), today), QString("Fri, Jan 1, 2027"));
        QCOMPARE(Dates::formatDue(today, 17 * 60, today), QString("Today, 5:00 PM"));

        // The year comes off cleanly however the language orders its dates.
        QCOMPARE(Dates::dayPattern(QLocale("en_US"), false), QString("ddd, MMM d"));
        QCOMPARE(Dates::dayPattern(QLocale("de_DE"), false), QString("ddd, d. MMM"));
        QCOMPARE(Dates::dayPattern(QLocale("ja_JP"), false), QString("M月d日ddd"));
        for (const char *name : {"da_DK", "fr_FR", "es_MX", "pt_BR", "hu_HU", "ko_KR", "ru_RU", "ar_AE"}) {
            const QString pattern = Dates::dayPattern(QLocale(name), false);
            QVERIFY2(!pattern.contains('y') && pattern.contains('d') && pattern.contains('M'), name);
        }
    }

    // ---- dates (review fixes) ----

private:
    // Quick entry typed on the tests' Friday, at another time of day.
    static QuickEntry typedAt(const QString &text, int hour, int minute = 0) {
        return Dates::parseQuick(text, QDateTime(friday.date(), QTime(hour, minute)));
    }

private slots:
    void tonightIsTheEveningAhead() {
        // At seven in the evening, "tonight at 9" is two hours off, not ten hours gone.
        QuickEntry entry = typedAt("Call mom tonight at 9", 19);
        QCOMPARE(entry.title, QString("Call mom"));
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 21 * 60);
        // Typed in the morning, it is not twelve hours early either.
        QCOMPARE(typedAt("Call mom tonight at 9", 8).minutes, 21 * 60);
        QCOMPARE(typedAt("Call mom at 9 tonight", 19).minutes, 21 * 60);
        QCOMPARE(typedAt("Call mom tonight at 11:30", 19).minutes, 23 * 60 + 30);
        entry = typedAt("Call mom this evening at 9", 19);
        QCOMPARE(entry.title, QString("Call mom"));
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 21 * 60);
        entry = typedAt("Movie this evening", 10);
        QCOMPARE(entry.title, QString("Movie"));
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 20 * 60);

        // A time that says which half of the day it is keeps it.
        QCOMPARE(typedAt("Call mom tonight at 5", 10).minutes, 17 * 60);
        QCOMPARE(typedAt("Call mom tonight at 7pm", 10).minutes, 19 * 60);
        QCOMPARE(typedAt("Call mom tonight at 21:30", 10).minutes, 21 * 60 + 30);
        // Midnight and the small hours end the night, on the next day.
        entry = typedAt("Lock up tonight at midnight", 19);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 0);
        entry = typedAt("Lock up tonight at 12", 19);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 0);
        entry = typedAt("Check the oven tonight at 1am", 19);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 60);
        // Still up after midnight, tonight is the night that is not over yet.
        entry = typedAt("Check the oven tonight at 1", 0, 30);
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 60);

        // A new reminder's line reads it that way as it is typed.
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return QDateTime(friday.date(), QTime(19, 0)); });
        QCOMPARE(backend.quickPreview("Call mom tonight at 9"),
                 Dates::formatDue(friday.date(), 21 * 60, friday.date()));
    }

    void tonightAlertsInTheEvening() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        // A Friday evening still ahead, as the store dates its changes by the real clock.
        const QDate day(2030, 5, 3);
        backend.setClock([day] { return QDateTime(day, QTime(19, 0)); });
        backend.setViewKey("list:" + QString::number(addList(store, "Home")));
        const qint64 id = backend.addReminder();
        backend.commitTitle(id, "Call mom tonight at 9");
        QCOMPARE(store.reminder(id)->dueMinutes, 21 * 60);

        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(QDateTime(day, QTime(20, 59)));
        QCOMPARE(alerts.sent.size(), 0);
        notifier.check(QDateTime(day, QTime(21, 0)));
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(alerts.sent.first().title, QString("Call mom"));
    }

    void aRepeatStartsAtItsNextMoment() {
        // On a Friday evening, this Friday's nine o'clock has gone by.
        QuickEntry entry = typedAt("Team sync every friday at 9", 19, 12);
        QCOMPARE(entry.title, QString("Team sync"));
        QCOMPARE(entry.repeat, QString("weekly:1"));
        QCOMPARE(entry.due, QDate(2026, 11, 20));
        QCOMPARE(entry.minutes, 9 * 60);
        entry = typedAt("Team sync every other friday at 9", 19, 12);
        QCOMPARE(entry.repeat, QString("weekly:2"));
        QCOMPARE(entry.due, QDate(2026, 11, 20));
        // Before nine, or without a time, it starts today.
        QCOMPARE(typedAt("Team sync every friday at 9", 8).due, friday.date());
        QCOMPARE(typedAt("Team sync every friday", 19, 12).due, friday.date());
        QCOMPARE(typedAt("Team sync every monday at 9", 19, 12).due, QDate(2026, 11, 16));
    }

    void minutesKeepTheirHoursHalfOfTheDay() {
        // "at 5:30" is read the way "at 5" is: in the afternoon.
        QCOMPARE(typedAt("Call at 5:30", 10).minutes, 17 * 60 + 30);
        QCOMPARE(typedAt("Call at 5", 10).minutes, 17 * 60);
        QCOMPARE(typedAt("Call at 7:45", 10).minutes, 19 * 60 + 45);
        QCOMPARE(typedAt("Call at 9:30", 8).minutes, 9 * 60 + 30);
        QCOMPARE(typedAt("Call at 12:15", 8).minutes, 12 * 60 + 15);
        QCOMPARE(typedAt("Call at 5:30am", 4).minutes, 5 * 60 + 30);
        // The 24-hour clock means what it says.
        QCOMPARE(typedAt("Call at 05:30", 4).minutes, 5 * 60 + 30);
        QCOMPARE(typedAt("Call at 17:30", 10).minutes, 17 * 60 + 30);
        QCOMPARE(typedAt("Call at 0:30", 10).minutes, 30);
        // The time field reads them the same way.
        QCOMPARE(Dates::parseTime("5:30"), 17 * 60 + 30);
        QCOMPARE(Dates::parseTime("9:30"), 9 * 60 + 30);
        QCOMPARE(Dates::parseTime("05:30"), 5 * 60 + 30);
        QCOMPARE(Dates::parseTime("05"), 5 * 60);
    }

    void littleWordsStayInTheTitleUnlessTheDateHasThem() {
        QuickEntry entry = typedAt("Finish this tomorrow", 10);
        QCOMPARE(entry.title, QString("Finish this"));
        QCOMPARE(entry.due, QDate(2026, 11, 14));
        QCOMPARE(typedAt("Do this every day", 10).title, QString("Do this"));
        QCOMPARE(typedAt("Read this next week", 10).title, QString("Read this"));
        QCOMPARE(typedAt("Fix this dec 1", 10).title, QString("Fix this"));
        QCOMPARE(typedAt("Follow up on this tomorrow", 10).title, QString("Follow up on this"));
        QCOMPARE(typedAt("Move on tomorrow", 10).title, QString("Move on"));
        QCOMPARE(typedAt("Stop by every friday", 10).title, QString("Stop by"));
        QCOMPARE(typedAt("Stand by in 5 minutes", 10).title, QString("Stand by"));

        // Where they are part of the date, they go with it.
        entry = typedAt("Call mom this friday", 10);
        QCOMPARE(entry.title, QString("Call mom"));
        QCOMPARE(entry.due, QDate(2026, 11, 20));
        entry = typedAt("Lunch on sun", 10);
        QCOMPARE(entry.title, QString("Lunch"));
        QCOMPARE(entry.due, QDate(2026, 11, 15));
        QCOMPARE(typedAt("Send the report by tomorrow", 10).title, QString("Send the report"));
        QCOMPARE(typedAt("Stay in until next week", 10).title, QString("Stay in"));
        QCOMPARE(typedAt("Dinner on nov 20", 10).title, QString("Dinner"));
        QCOMPARE(typedAt("Taxes due apr 15", 10).title, QString("Taxes"));
        QCOMPARE(typedAt("Library books due in 3 days", 10).title, QString("Library books"));
        QCOMPARE(typedAt("Rent due every month", 10).title, QString("Rent"));
        // A date field has no title to keep them for.
        QCOMPARE(Dates::parseDate("on tomorrow", friday.date()), QDate(2026, 11, 14));
    }

    void aNumberWithALetterIsNotAlwaysATime() {
        // "2a" is a room, a seat, or a step as often as two in the morning.
        QuickEntry entry = typedAt("Meet in room 2a tomorrow", 10);
        QCOMPARE(entry.title, QString("Meet in room 2a"));
        QCOMPARE(entry.due, QDate(2026, 11, 14));
        QCOMPARE(entry.minutes, -1);
        entry = typedAt("Swap seat 12a", 10);
        QCOMPARE(entry.title, QString("Swap seat 12a"));
        QVERIFY(!entry.due.isValid());
        // After "at", or with its minutes, it is a time; and "7p" is the evening.
        QCOMPARE(typedAt("Breakfast at 7a", 6).minutes, 7 * 60);
        QCOMPARE(typedAt("Breakfast 7:30a", 6).minutes, 7 * 60 + 30);
        QCOMPARE(typedAt("Dinner 7p", 10).minutes, 19 * 60);
        QCOMPARE(Dates::parseTime("7a"), 7 * 60);
    }

    void aLeapDayIsTheNextOne() {
        const QDate today = friday.date();
        QCOMPARE(Dates::parseDate("2/29", today), QDate(2028, 2, 29));
        QCOMPARE(Dates::parseDate("feb 29", today), QDate(2028, 2, 29));
        QCOMPARE(Dates::parseDate("2/29", QDate(2028, 2, 29)), QDate(2028, 2, 29));
        QCOMPARE(Dates::parseDate("2/29", QDate(2028, 3, 1)), QDate(2032, 2, 29));
        QCOMPARE(Dates::parseQuick("Leap day party feb 29", friday).due, QDate(2028, 2, 29));
        // Or the one in the year given, if there is one.
        QCOMPARE(Dates::parseDate("2/29/2028", today), QDate(2028, 2, 29));
        QVERIFY(!Dates::parseDate("2/29/2027", today).isValid());
        QCOMPARE(Dates::parseQuick("Leap day party feb 29 2032", friday).due, QDate(2032, 2, 29));
        // Other days come round once a year, as before.
        QCOMPARE(Dates::parseDate("3/1", QDate(2028, 3, 2)), QDate(2029, 3, 1));
        QCOMPARE(Dates::parseQuick("Leap day party feb 28", QDateTime(QDate(2028, 3, 1), QTime(10, 0))).due,
                 QDate(2029, 2, 28));

        // Where the day comes first.
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(Dates::parseDate("29.2.", today), QDate(2028, 2, 29));
        QCOMPARE(Dates::parseDate("29/2", today), QDate(2028, 2, 29));
    }

    // ---- The store ---------------------------------------------------------------

    void storeKeepsWhatItIsGiven() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        qint64 id = 0;
        {
            Store store(path);
            QVERIFY2(store.isOpen(), qPrintable(store.error()));
            const qint64 listId = addList(store, "Home");
            Reminder r;
            r.listId = listId;
            r.title = "Renew passport";
            r.notes = "Photos are in the drawer";
            r.due = QDate(2026, 12, 1);
            r.dueMinutes = 9 * 60;
            r.repeat = "yearly:1:1";
            r.priority = 3;
            r.flagged = true;
            r.tags = {"travel", "papers"};
            id = store.put(r);
            QVERIFY(id > 0);
        }
        Store store(path);
        const Reminder *r = store.reminder(id);
        QVERIFY(r);
        QCOMPARE(r->title, QString("Renew passport"));
        QCOMPARE(r->notes, QString("Photos are in the drawer"));
        QCOMPARE(r->due, QDate(2026, 12, 1));
        QCOMPARE(r->dueMinutes, 9 * 60);
        QCOMPARE(r->repeat, QString("yearly:1:1"));
        QCOMPARE(r->priority, 3);
        QVERIFY(r->flagged);
        QCOMPARE(r->tags, (QStringList{"travel", "papers"}));
        QCOMPARE(store.lists().size(), 1);
    }

    void storeSaysWhenItCannotOpen() {
        Store store("/proc/nudge/nudge.db");
        QVERIFY(!store.isOpen());
        QVERIFY(!store.error().isEmpty());
        QCOMPARE(store.put(Reminder()), 0);
    }

    void undoAndRedoGoStepByStep() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Milk");
        Reminder changed = *store.reminder(id);
        changed.title = "Oat milk";
        store.put(changed);

        QVERIFY(store.canUndo());
        store.undo();
        QCOMPARE(store.reminder(id)->title, QString("Milk"));
        store.undo();
        QVERIFY(!store.reminder(id));
        store.redo();
        store.redo();
        QCOMPARE(store.reminder(id)->title, QString("Oat milk"));
        QVERIFY(!store.canRedo());

        // And the file agrees with memory after all that.
        Store again(store.path());
        QCOMPARE(again.reminder(id)->title, QString("Oat milk"));
    }

    void changesInOneTransactionUndoTogether() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        {
            Store::Tx tx(&store);
            add(store, listId, "One");
            add(store, listId, "Two");
        }
        QCOMPARE(store.reminders().size(), 2);
        store.undo();
        QCOMPARE(store.reminders().size(), 0);
    }

    void typingMergesIntoOneUndoStep() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 id = add(store, addList(store, "Home"), "Milk");
        for (const char *notes : {"S", "So", "Soy"}) {
            Store::Tx tx(&store, "notes:1");
            Reminder r = *store.reminder(id);
            r.notes = notes;
            store.put(r);
        }
        QCOMPARE(store.reminder(id)->notes, QString("Soy"));
        store.undo();
        QCOMPARE(store.reminder(id)->notes, QString());

        // Once sealed, the same field starts a new step.
        store.redo();
        store.sealUndo();
        {
            Store::Tx tx(&store, "notes:1");
            Reminder r = *store.reminder(id);
            r.notes = "Soy, unsweetened";
            store.put(r);
        }
        store.undo();
        QCOMPARE(store.reminder(id)->notes, QString("Soy"));
    }

    void deletingAListTakesItsRemindersAndUndoBringsThemBack() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        add(store, home, "One");
        const qint64 parent = add(store, home, "Two");
        Reminder child;
        child.listId = home;
        child.parentId = parent;
        child.title = "Two and a half";
        store.put(child);
        add(store, work, "Three");
        Section section;
        section.listId = home;
        section.name = "Later";
        store.put(section);

        store.removeList(home);
        QCOMPARE(store.lists().size(), 1);
        QCOMPARE(store.reminders().size(), 1);
        QCOMPARE(store.sections().size(), 0);
        store.undo();
        QCOMPARE(store.lists().size(), 2);
        QCOMPARE(store.reminders().size(), 4);
        QCOMPARE(store.sections().size(), 1);
        QCOMPARE(store.childrenOf(parent).size(), 1);
    }

    void completingARepeatingReminderMovesItOn() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        Reminder r;
        r.listId = listId;
        r.title = "Water the plants";
        r.due = QDate(2026, 11, 6); // a week overdue
        r.repeat = "weekly:1";
        const qint64 id = store.put(r);

        const QDate next = store.complete(id, friday);
        QCOMPARE(next, friday.date());
        QVERIFY(!store.reminder(id)->completed);
        QCOMPARE(store.reminder(id)->due, friday.date());
        // What was done is on record.
        QCOMPARE(store.reminders().size(), 2);
        int done = 0;
        for (const Reminder &other : store.reminders())
            done += other.completed && other.repeat.isEmpty() && other.due == QDate(2026, 11, 6);
        QCOMPARE(done, 1);

        // One step back undoes both halves.
        store.undo();
        QCOMPARE(store.reminders().size(), 1);
        QCOMPARE(store.reminder(id)->due, QDate(2026, 11, 6));
    }

    void aRepeatThatHasEndedJustCompletes() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = "Course";
        r.due = friday.date();
        r.repeat = "weekly:1";
        r.repeatEnd = QDate(2026, 11, 15);
        const qint64 id = store.put(r);
        QVERIFY(!store.complete(id, friday).isValid());
        QVERIFY(store.reminder(id)->completed);
    }

    void completingAParentCompletesItsSubtasks() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 parent = add(store, listId, "Trip");
        Reminder child;
        child.listId = listId;
        child.parentId = parent;
        child.title = "Book the cabin";
        const qint64 childId = store.put(child);

        store.complete(parent, friday);
        QVERIFY(store.reminder(childId)->completed);
        // Reopening the subtask reopens what it belongs to.
        store.reopen(childId);
        QVERIFY(!store.reminder(parent)->completed);
    }

    void anotherProcessShowsUp() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), commandLine(path);
        const qint64 listId = addList(window, "Home");
        QVERIFY(commandLine.reloadIfChanged());
        QSignalSpy changed(&window, &Store::changed);
        const qint64 id = add(commandLine, listId, "From the command line");
        QVERIFY(window.reloadIfChanged());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(window.reminder(id)->title, QString("From the command line"));

        // Alert bookkeeping alone does not disturb the window or its undo.
        add(window, listId, "Mine");
        QVERIFY(window.canUndo());
        commandLine.reloadIfChanged();
        commandLine.setAlerted(id, 123, 0, 0);
        QVERIFY(!window.reloadIfChanged());
        QVERIFY(window.canUndo());
        QCOMPARE(window.reminder(id)->alertedDue, 123);
    }

    // ---- store (review fixes) ----

private:
    // Changes the file behind the store's back, as another program would.
    static void tamper(const QString &path, const QString &sql) {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "tamper");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec(sql));
        }
        QSqlDatabase::removeDatabase("tamper");
    }

    static void retitle(Store &store, qint64 id, const QString &title, const QString &mergeKey = {}) {
        Store::Tx tx(&store, mergeKey);
        Reminder r = *store.reminder(id);
        r.title = title;
        store.put(r);
    }

private slots:
    void aReloadReadsOneMomentOfTheFile() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), commandLine(path);
        const qint64 home = addList(commandLine, "Home");
        // Part way through the window's reading, the command line adds a
        // list with a reminder in it.
        qint64 work = 0, report = 0;
        bool once = true;
        window.setLoadHook([&] {
            if (!std::exchange(once, false))
                return;
            Store::Tx tx(&commandLine);
            work = addList(commandLine, "Work");
            report = add(commandLine, work, "Send the report");
        });
        QVERIFY(window.reloadIfChanged());
        QVERIFY(window.list(home));
        // It shows up whole the next time, not half of it now and the rest never.
        QVERIFY(!window.reminder(report));
        QVERIFY(window.reloadIfChanged());
        QVERIFY(window.list(work));
        QVERIFY(window.reminder(report));
        QCOMPARE(window.reminder(report)->listId, work);
    }

    void aCommandThatOnlyReadsLeavesOthersAlone() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path);
        add(window, addList(window, "Home"), "Milk");
        int loads = 0;
        window.setLoadHook([&loads] { ++loads; });
        {
            Store list(path); // nudge list
            QCOMPARE(list.reminders().size(), 1);
        }
        QVERIFY(!window.reloadIfChanged());
        QCOMPARE(loads, 0);
    }

    void aDeleteThatFailsChangesNothing() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        const qint64 id = add(store, addList(store, "Home"), "Milk");
        store.clearUndo();
        tamper(path, "CREATE TRIGGER refuse BEFORE DELETE ON reminders BEGIN SELECT RAISE(ABORT, 'disk is full'); END");
        QSignalSpy failed(&store, &Store::failed);
        store.removeReminder(id);
        QVERIFY(store.reminder(id));
        QVERIFY(!store.canUndo());
        QCOMPARE(failed.count(), 1);
        QVERIFY(store.error().contains("disk is full"));

        // Deleting a list goes the same way when only the list itself is refused.
        const qint64 listId = store.reminder(id)->listId;
        Section section;
        section.listId = listId;
        section.name = "Later";
        const qint64 sectionId = store.put(section);
        store.clearUndo();
        tamper(path, "DROP TRIGGER refuse");
        tamper(path, "CREATE TRIGGER refuse BEFORE DELETE ON lists BEGIN SELECT RAISE(ABORT, 'disk is full'); END");
        store.removeList(listId);
        QVERIFY(store.list(listId));
        QVERIFY(store.section(sectionId));
        QVERIFY(store.reminder(id));
        QVERIFY(!store.canUndo());
        Store again(path);
        QVERIFY(again.list(listId) && again.section(sectionId) && again.reminder(id));
    }

    void aWriteThatFailsTakesItsWholeStepBack() {
        // SQLite refuses the second of three writes; with ROLLBACK it ends
        // the transaction as well, as it can when the disk is full.
        for (const QString &resolution : {QString("ABORT"), QString("ROLLBACK")}) {
            QTemporaryDir dir;
            const QString path = dir.filePath("nudge.db");
            Store store(path);
            const qint64 listId = addList(store, "Home");
            const QVector<qint64> ids{add(store, listId, "One"), add(store, listId, "Two"), add(store, listId, "Three")};
            store.clearUndo();
            tamper(path, QString("CREATE TRIGGER refuse BEFORE INSERT ON reminders WHEN NEW.title = 'Two!' "
                                 "BEGIN SELECT RAISE(%1, 'disk is full'); END")
                             .arg(resolution));
            QSignalSpy failed(&store, &Store::failed);
            {
                Store::Tx tx(&store);
                for (const qint64 id : ids) {
                    Reminder r = *store.reminder(id);
                    r.title += "!";
                    store.put(r);
                }
            }
            QCOMPARE(failed.count(), 1);
            QVERIFY(!store.canUndo());
            Store again(path);
            for (const Store *seen : {&store, &again}) {
                QCOMPARE(seen->reminder(ids[0])->title, QString("One"));
                QCOMPARE(seen->reminder(ids[1])->title, QString("Two"));
                QCOMPARE(seen->reminder(ids[2])->title, QString("Three"));
            }
            // On its own, a write that fails says so by returning zero.
            Reminder r = *store.reminder(ids[1]);
            r.title = "Two!";
            QCOMPARE(store.put(r), 0);
            QCOMPARE(store.reminder(ids[1])->title, QString("Two"));
        }
    }

    void anUndoThatFailsKeepsItsStep() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        const qint64 id = add(store, addList(store, "Home"), "Milk");
        retitle(store, id, "Oat milk");
        tamper(path, "CREATE TRIGGER refuse BEFORE INSERT ON reminders WHEN NEW.title = 'Milk' "
                     "BEGIN SELECT RAISE(ABORT, 'disk is full'); END");
        QSignalSpy failed(&store, &Store::failed);
        store.undo();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(store.reminder(id)->title, QString("Oat milk"));
        QVERIFY(store.canUndo());
        QVERIFY(!store.canRedo());
        tamper(path, "DROP TRIGGER refuse");
        store.undo();
        QVERIFY(store.reminder(id));
        QCOMPARE(store.reminder(id)->title, QString("Milk"));
    }

    void editingAfterUndoOrRedoStartsANewStep() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Milk");
        auto note = [&store, id](const QString &notes) {
            Store::Tx tx(&store, "notes:1");
            Reminder r = *store.reminder(id);
            r.notes = notes;
            store.put(r);
        };
        note("Soy");
        retitle(store, id, "Soy milk", "title:1");
        store.undo(); // the title, which leaves the notes on top
        note("Soy, unsweetened");
        store.undo();
        QCOMPARE(store.reminder(id)->notes, QString("Soy"));

        store.redo();
        note("Soy, unsweetened, cold");
        store.undo();
        QCOMPARE(store.reminder(id)->notes, QString("Soy, unsweetened"));

        // The same when another process's edit takes away the step on top.
        const qint64 eggs = add(store, listId, "Eggs");
        note("Soy, warm");
        retitle(store, eggs, "Brown eggs", "title:2");
        Store commandLine(store.path());
        retitle(commandLine, eggs, "Free-range eggs");
        QVERIFY(store.reloadIfChanged());
        note("Soy, warm, frothed");
        store.undo();
        QCOMPARE(store.reminder(id)->notes, QString("Soy, warm"));
    }

    void undoAndRedoDoNotAnnounceAMomentAgain() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const QDateTime ten(QDate(2030, 5, 1), QTime(10, 0));
        const qint64 id = add(store, addList(store, "Home"), "Stretch", ten.date(), 9 * 60);
        Reminder r = *store.reminder(id);
        r.dueMinutes = 10 * 60;
        store.put(r);
        notifier.check(ten);
        QCOMPARE(alerts.sent.size(), 1);

        // Back to nine and on to ten again: ten has been announced already.
        store.undo();
        QCOMPARE(store.reminder(id)->dueMinutes, 9 * 60);
        store.redo();
        QCOMPARE(store.reminder(id)->dueMinutes, 10 * 60);
        notifier.check(ten.addSecs(60));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void anUndoneReminderCountsAsJustEdited() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        qint64 id = 0;
        {
            Store store(path);
            id = add(store, addList(store, "Home"), "Stretch");
        }
        tamper(path, "UPDATE reminders SET modified_at = 1");
        Store store(path);
        retitle(store, id, "Stretch the back");
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        store.undo();
        QCOMPARE(store.reminder(id)->title, QString("Stretch"));
        QVERIFY(store.reminder(id)->modifiedAt >= before);

        // A snooze that ran out while it was deleted is not announced late.
        store.setAlerted(id, 0, 0, before - 60);
        store.removeReminder(id);
        store.undo();
        QVERIFY(store.reminder(id));
        QCOMPARE(store.reminder(id)->snoozeUntil, 0);
    }

    void anotherProcessCostsOnlyTheStepsItTouches() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), commandLine(path);
        const qint64 home = addList(window, "Home");
        const qint64 milk = add(window, home, "Milk"), eggs = add(window, home, "Eggs", QDate(2030, 5, 1), 9 * 60);
        retitle(window, milk, "Oat milk");
        retitle(window, eggs, "Brown eggs");

        // The background service announcing Eggs and snoozing it is no edit.
        QVERIFY(commandLine.reloadIfChanged());
        FakeAlerts alerts;
        Notifier background(&commandLine, &alerts);
        const QDateTime due(QDate(2030, 5, 1), QTime(9, 0));
        background.check(due);
        background.act(1, "hour", due);
        QCOMPARE(alerts.sent.size(), 1);
        QVERIFY(commandLine.reminder(eggs)->snoozeUntil > 0);
        QVERIFY(!window.reloadIfChanged());
        QVERIFY(window.canUndo());

        // The command line renames Eggs and adds Bread to Home. What the
        // window did to Milk can still be undone; what it did to Eggs, or
        // making the list Bread is now in, cannot.
        retitle(commandLine, eggs, "Free-range eggs");
        const qint64 bread = add(commandLine, home, "Bread");
        QVERIFY(window.reloadIfChanged());
        QVERIFY(window.canUndo());
        window.undo();
        QCOMPARE(window.reminder(milk)->title, QString("Milk"));
        window.undo();
        QVERIFY(!window.reminder(milk));
        QVERIFY(!window.canUndo());
        QCOMPARE(window.reminder(eggs)->title, QString("Free-range eggs"));
        QVERIFY(window.list(home));
        QCOMPARE(window.reminder(bread)->listId, home);
    }

    void redoDoesNotDeleteWhatAnotherProcessFilledIn() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), commandLine(path);
        const qint64 home = addList(window, "Home"), work = addList(window, "Work");
        const qint64 milk = add(window, home, "Milk");
        window.removeList(work);
        window.undo(); // Work is back, and deleting it waits to be redone
        QVERIFY(commandLine.reloadIfChanged());
        const qint64 report = add(commandLine, work, "Send the report");
        QVERIFY(window.reloadIfChanged());
        QVERIFY(!window.canRedo());
        window.undo();
        QVERIFY(!window.reminder(milk));
        QVERIFY(window.list(work));
        QCOMPARE(window.reminder(report)->listId, work);
    }

    void undoNeverWritesOverAnotherProcess() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), commandLine(path);
        const qint64 home = addList(window, "Home");
        const qint64 milk = add(window, home, "Milk"), eggs = add(window, home, "Eggs");
        retitle(window, milk, "Oat milk");
        retitle(window, eggs, "Brown eggs");
        window.undo(); // Eggs waits to be redone

        // The command line edits both before the window has caught up.
        QVERIFY(commandLine.reloadIfChanged());
        for (const qint64 id : {milk, eggs}) {
            Reminder theirs = *commandLine.reminder(id);
            theirs.notes = "From the command line";
            commandLine.put(theirs);
        }
        window.undo();
        window.redo();
        QVERIFY(!window.canUndo());
        QVERIFY(!window.canRedo());
        Store again(path);
        for (const Store *seen : {&window, &again}) {
            QCOMPARE(seen->reminder(milk)->title, QString("Oat milk"));
            QCOMPARE(seen->reminder(milk)->notes, QString("From the command line"));
            QCOMPARE(seen->reminder(eggs)->title, QString("Eggs"));
            QCOMPARE(seen->reminder(eggs)->notes, QString("From the command line"));
        }
    }

    void aTimedRepeatDoneLateMovesPastToday() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        auto weekly = [&](const QString &title, int minutes) {
            Reminder r;
            r.listId = listId;
            r.title = title;
            r.due = QDate(2026, 11, 6); // a week overdue
            r.dueMinutes = minutes;
            r.repeat = "weekly:1";
            return store.put(r);
        };
        // Done at ten on Friday: nine has gone by today, three has not.
        QCOMPARE(store.complete(weekly("Water the plants", 9 * 60), friday), QDate(2026, 11, 20));
        QCOMPARE(store.complete(weekly("Take out the bins", 15 * 60), friday), friday.date());
        QCOMPARE(store.complete(weekly("Call home", -1), friday), friday.date());

        const Repeat daily = Repeat::parse("daily:1");
        QCOMPARE(daily.upcoming(QDate(2026, 10, 1), 10 * 60, friday), QDate(2026, 11, 14)); // ten is not ahead
        QCOMPARE(daily.upcoming(QDate(2026, 10, 1), 10 * 60 + 1, friday), friday.date());
        QCOMPARE(daily.upcoming(QDate(2026, 10, 1), -1, friday), friday.date());
        QVERIFY(!daily.upcoming(QDate(2026, 11, 12), 9 * 60, friday, friday.date()).isValid());
    }

    void anOpenSubtaskReopensItsParent() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        store.complete(trip, friday);
        Reminder cabin;
        cabin.listId = listId;
        cabin.parentId = trip;
        cabin.title = "Book the cabin";
        const qint64 cabinId = store.put(cabin);
        QVERIFY(cabinId > 0);
        QVERIFY(!store.reminder(trip)->completed);
        // Adding it and reopening the trip are one step.
        store.undo();
        QVERIFY(!store.reminder(cabinId));
        QVERIFY(store.reminder(trip)->completed);
        store.redo();
        QVERIFY(store.reminder(cabinId));
        QVERIFY(!store.reminder(trip)->completed);
    }

    void openingRepairsAParentDoneBeforeItsSubtasks() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        qint64 trip = 0, cabin = 0;
        {
            Store store(path);
            const qint64 listId = addList(store, "Home");
            trip = add(store, listId, "Trip");
            Reminder child;
            child.listId = listId;
            child.parentId = trip;
            child.title = "Book the cabin";
            cabin = store.put(child);
        }
        // As Nudge 0.1.0 could leave it.
        tamper(path, QString("UPDATE reminders SET completed = 1, completed_at = 1 WHERE id = %1").arg(trip));
        Store store(path);
        QVERIFY(!store.reminder(trip)->completed);
        QVERIFY(!store.reminder(cabin)->completed);
        QVERIFY(!store.canUndo());
        Store again(path);
        QVERIFY(!again.reminder(trip)->completed);
    }

    // ---- Views -------------------------------------------------------------------

    void aListShowsSectionsAndSubtasks() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        Reminder child;
        child.listId = listId;
        child.parentId = trip;
        child.title = "Book the cabin";
        store.put(child);
        Section section;
        section.listId = listId;
        section.name = "Later";
        const qint64 later = store.put(section);
        Reminder inSection;
        inSection.listId = listId;
        inSection.sectionId = later;
        inSection.title = "Paint the fence";
        store.put(inSection);
        const qint64 done = add(store, listId, "Already done");
        store.complete(done, friday);

        ViewOptions options;
        options.now = friday;
        const View view{View::List, listId, {}};
        QCOMPARE(titles(store, buildRows(store, view, options)),
                 (QStringList{"Trip", "  Book the cabin", "[Later]", "Paint the fence"}));

        options.showCompleted = true;
        QCOMPARE(titles(store, buildRows(store, view, options)),
                 (QStringList{"Trip", "  Book the cabin", "Already done", "[Later]", "Paint the fence"}));

        options.showCompleted = false;
        options.collapsed = {trip};
        const QVector<Row> rows = buildRows(store, view, options);
        QCOMPARE(titles(store, rows), (QStringList{"Trip", "[Later]", "Paint the fence"}));
        QCOMPARE(rows[0].children, 1);
        QVERIFY(rows[0].collapsed);
    }

    void aListSortsTheWayItIsAsked() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        add(store, listId, "Cherries", QDate(2026, 12, 3));
        add(store, listId, "apples");
        add(store, listId, "Bananas", QDate(2026, 11, 20));
        ViewOptions options;
        options.now = friday;
        const View view{View::List, listId, {}};
        QCOMPARE(titles(store, buildRows(store, view, options)), (QStringList{"Cherries", "apples", "Bananas"}));

        TaskList list = *store.list(listId);
        list.sort = "title";
        store.put(list);
        QCOMPARE(titles(store, buildRows(store, view, options)), (QStringList{"apples", "Bananas", "Cherries"}));
        list.sort = "due";
        store.put(list);
        QCOMPARE(titles(store, buildRows(store, view, options)), (QStringList{"Bananas", "Cherries", "apples"}));
    }

    void todaySetsOverdueApart() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        add(store, listId, "Afternoon", friday.date(), 15 * 60);
        add(store, listId, "All day", friday.date());
        add(store, listId, "Tomorrow", friday.date().addDays(1));
        add(store, listId, "No date");
        ViewOptions options;
        options.now = friday;
        // With nothing overdue there is nothing to set apart, so no headings.
        QCOMPARE(titles(store, buildRows(store, View{View::Today, 0, {}}, options)),
                 (QStringList{"All day", "Afternoon"}));

        add(store, listId, "Last week", friday.date().addDays(-7));
        QCOMPARE(titles(store, buildRows(store, View{View::Today, 0, {}}, options)),
                 (QStringList{"[Overdue]", "Last week", "[Due Today]", "All day", "Afternoon"}));
    }

    void scheduledGroupsByDayThenMonth() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        add(store, listId, "Late", QDate(2026, 11, 10));
        add(store, listId, "Now", friday.date());
        add(store, listId, "Soon", QDate(2026, 11, 16));
        add(store, listId, "This month", QDate(2026, 11, 28));
        add(store, listId, "Next month", QDate(2026, 12, 9));
        add(store, listId, "Also next month", QDate(2026, 12, 20));
        add(store, listId, "Undated");
        ViewOptions options;
        options.now = friday;
        QCOMPARE(titles(store, buildRows(store, View{View::Scheduled, 0, {}}, options)),
                 (QStringList{"[Overdue]", "Late", "[Today]", "Now", "[Monday]", "Soon", "[November 2026]",
                              "This month", "[December 2026]", "Next month", "Also next month"}));
    }

    void otherViewsGatherFromEveryList() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        const qint64 flagged = add(store, home, "Flagged one");
        Reminder r = *store.reminder(flagged);
        r.flagged = true;
        r.tags = {"Errand"};
        r.notes = "Ask about the invoice";
        store.put(r);
        add(store, work, "Send the report");
        const qint64 done = add(store, work, "Filed");
        store.complete(done, friday);

        ViewOptions options;
        options.now = friday;
        QCOMPARE(titles(store, buildRows(store, View{View::Flagged, 0, {}}, options)), (QStringList{"Flagged one"}));
        QCOMPARE(titles(store, buildRows(store, View{View::Tag, 0, "errand"}, options)), (QStringList{"Flagged one"}));
        QCOMPARE(titles(store, buildRows(store, View{View::Completed, 0, {}}, options)), (QStringList{"[Today]", "Filed"}));
        QCOMPARE(titles(store, buildRows(store, View{View::All, 0, {}}, options)),
                 (QStringList{"[Home]", "Flagged one", "[Work]", "Send the report"}));
        // Search looks in notes and tags too, and every word has to match.
        QCOMPARE(titles(store, buildRows(store, View{View::Search, 0, "invoice"}, options)), (QStringList{"Flagged one"}));
        QCOMPARE(titles(store, buildRows(store, View{View::Search, 0, "errand flagged"}, options)),
                 (QStringList{"Flagged one"}));
        QCOMPARE(titles(store, buildRows(store, View{View::Search, 0, "errand report"}, options)), QStringList());
    }

    void theModelChangesOnlyWhatChanged() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        const qint64 a = add(store, listId, "A");
        const qint64 b = add(store, listId, "B");
        const qint64 c = add(store, listId, "C");
        ReminderModel model(&store);
        ViewOptions options;
        options.now = friday;
        const View view{View::List, listId, {}};
        model.show(buildRows(store, view, options), true);
        QCOMPARE(model.rowCount(), 3);

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);

        add(store, listId, "D");
        model.show(buildRows(store, view, options), false);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.first().at(1).toInt(), 3);

        store.removeReminder(b);
        model.show(buildRows(store, view, options), false);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.first().at(1).toInt(), 1);

        Reminder first = *store.reminder(c);
        first.position = store.reminder(a)->position - 1;
        store.put(first);
        model.show(buildRows(store, view, options), false);
        QCOMPARE(moved.count(), 1);
        QCOMPARE(model.idAt(0), c);
        QCOMPARE(model.rowOf(a), 1);

        QCOMPARE(reset.count(), 0);
        QCOMPARE(model.data(model.index(0), ReminderModel::TitleRole).toString(), QString("C"));
    }

    // ---- The window's actions ---------------------------------------------------------

    void aNewReminderIsReadAsItIsTyped() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return friday; });
        backend.setViewKey("list:" + QString::number(addList(store, "Home")));

        const qint64 id = backend.addReminder();
        QVERIFY(id > 0);
        QCOMPARE(backend.draftId(), id);
        QCOMPARE(backend.selectedId(), id);
        QCOMPARE(backend.quickPreview("Call mom tomorrow 5pm #family"), QString("Tomorrow, 5:00 PM · #family"));
        backend.commitTitle(id, "Call mom tomorrow 5pm #family");
        QCOMPARE(backend.draftId(), 0);
        QCOMPARE(store.reminder(id)->title, QString("Call mom"));
        QCOMPARE(store.reminder(id)->due, QDate(2026, 11, 14));
        QCOMPARE(store.reminder(id)->tags, QStringList{"family"});

        // Adding it and naming it are one thing to undo.
        backend.undo();
        QVERIFY(!store.reminder(id));
    }

    void anEmptyNewReminderLeavesNoTrace() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 first = add(store, listId, "First");
        backend.setViewKey("list:" + QString::number(listId));
        backend.setSelectedId(first);
        store.clearUndo();

        const qint64 id = backend.addReminder();
        backend.commitTitle(id, "   ");
        QVERIFY(!store.reminder(id));
        QVERIFY(!store.canUndo());
        QVERIFY(!store.canRedo());
        QCOMPARE(backend.selectedId(), first);
    }

    void newRemindersTakeAfterTheViewTheyAreAddedIn() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return friday; });
        const qint64 home = addList(store, "Home");
        const qint64 first = add(store, home, "First");
        add(store, home, "Last");

        backend.setViewKey("today");
        qint64 id = backend.addReminder();
        backend.commitTitle(id, "Due today");
        QCOMPARE(store.reminder(id)->due, friday.date());

        backend.setViewKey("flagged");
        id = backend.addReminder();
        backend.commitTitle(id, "Important");
        QVERIFY(store.reminder(id)->flagged);

        // In a list it lands right below the selected reminder.
        backend.setViewKey("list:" + QString::number(home));
        backend.setSelectedId(first);
        id = backend.addReminder();
        backend.commitTitle(id, "Second");
        QCOMPARE(backend.reminders()->rowOf(id), 1);

        // Nothing can be added to Completed, so it goes to a list.
        backend.setViewKey("completed");
        id = backend.addReminder();
        QCOMPARE(backend.viewKind(), QString("list"));
        backend.commitTitle(id, "Somewhere sensible");
        QVERIFY(store.reminder(id));
    }

    void completedRemindersLingerBeforeTheyLeave() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 one = add(store, listId, "One");
        const qint64 two = add(store, listId, "Two");
        backend.setViewKey("list:" + QString::number(listId));
        backend.setSelectedId(one);

        backend.toggleCompleted(one);
        QVERIFY(store.reminder(one)->completed);
        QVERIFY(backend.reminders()->rowOf(one) >= 0); // still there, to be taken back
        QTRY_VERIFY_WITH_TIMEOUT(backend.reminders()->rowOf(one) < 0, 4000);
        QCOMPARE(backend.selectedId(), two); // the selection moves on to what took its place

        backend.setShowCompleted(true);
        QVERIFY(backend.reminders()->rowOf(one) >= 0);
        backend.toggleCompleted(one);
        QVERIFY(!store.reminder(one)->completed);
        backend.setShowCompleted(false);
    }

    void subtasksIndentAndOutdent() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        const qint64 cabin = add(store, listId, "Book the cabin");
        const qint64 car = add(store, listId, "Pack the car");
        backend.setViewKey("list:" + QString::number(listId));

        backend.indent(cabin);
        backend.indent(car);
        QCOMPARE(store.childrenOf(trip), (QVector<qint64>{cabin, car}));
        // Subtasks go one level deep: the first reminder has nothing above it.
        backend.indent(trip);
        QCOMPARE(store.reminder(trip)->parentId, 0);

        backend.outdent(cabin);
        QCOMPARE(store.reminder(cabin)->parentId, 0);
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"Trip", "  Pack the car", "Book the cabin"}));
    }

    void remindersMoveUpDownAndAcrossSections() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 a = add(store, listId, "A");
        add(store, listId, "B");
        add(store, listId, "C");
        backend.setViewKey("list:" + QString::number(listId));
        const qint64 later = backend.addSection("Later");
        QVERIFY(later > 0);

        backend.moveReminder(a, 1);
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"B", "A", "C", "[Later]"}));
        backend.moveReminder(a, 1);
        backend.moveReminder(a, 1); // off the end of the unsectioned part, into the section
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"B", "C", "[Later]", "A"}));
        QCOMPARE(store.reminder(a)->sectionId, later);
        backend.moveReminder(a, -1);
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"B", "C", "A", "[Later]"}));

        // A sorted list says why it will not rearrange.
        backend.setSortMode("title");
        QSignalSpy notices(&backend, &Backend::notice);
        backend.moveReminder(a, -1);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"A", "B", "C", "[Later]"}));
    }

    void manyInsertionsInOneSpotKeepTheirOrder() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 first = add(store, listId, "First");
        add(store, listId, "Last");
        backend.setViewKey("list:" + QString::number(listId));
        // Each lands right below "First", halving the gap until it has to be respaced.
        for (int i = 0; i < 80; ++i) {
            backend.setSelectedId(first);
            backend.commitTitle(backend.addReminder(), QString("Item %1").arg(i));
        }
        const QStringList shown = titles(store, backend.reminders()->rows());
        QCOMPARE(shown.size(), 82);
        QCOMPARE(shown.first(), QString("First"));
        QCOMPARE(shown.at(1), QString("Item 79"));
        QCOMPARE(shown.at(80), QString("Item 0"));
        QCOMPARE(shown.last(), QString("Last"));
    }

    void movingToAnotherListTakesSubtasksAlong() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        const qint64 trip = add(store, home, "Trip");
        const qint64 cabin = add(store, home, "Book the cabin");
        backend.setViewKey("list:" + QString::number(home));
        backend.indent(cabin);

        backend.moveToList(trip, work);
        QCOMPARE(store.reminder(trip)->listId, work);
        QCOMPARE(store.reminder(cabin)->listId, work);
        QCOMPARE(store.reminder(cabin)->parentId, trip);
        backend.undo();
        QCOMPARE(store.reminder(cabin)->listId, home);
    }

    void theDetailsPaneSavesWhatItUnderstands() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return friday; });
        const qint64 id = add(store, addList(store, "Home"), "Dentist");
        QSignalSpy notices(&backend, &Backend::notice);

        QVERIFY(backend.setDate(id, "next friday"));
        QCOMPARE(store.reminder(id)->due, QDate(2026, 11, 20));
        QVERIFY(!backend.setDate(id, "whenever"));
        QCOMPARE(notices.count(), 1); // and it says so
        QCOMPARE(store.reminder(id)->due, QDate(2026, 11, 20));

        QVERIFY(backend.setTime(id, "5pm"));
        QCOMPARE(store.reminder(id)->dueMinutes, 17 * 60);
        backend.setRepeat(id, "monthly", 2);
        QCOMPARE(store.reminder(id)->repeat, QString("monthly:2:20"));
        // Moving the date moves the day a monthly repeat returns to.
        backend.setDateIso(id, "2026-11-25");
        QCOMPARE(store.reminder(id)->repeat, QString("monthly:2:25"));
        backend.setTags(id, "#Health, teeth health");
        QCOMPARE(store.reminder(id)->tags, (QStringList{"Health", "teeth"}));
        backend.setEarly(id, 30);
        backend.setPriority(id, 3);
        QCOMPARE(store.reminder(id)->early, 30);
        QCOMPARE(store.reminder(id)->priority, 3);

        backend.setSelectedId(id);
        const QVariantMap selected = backend.selected();
        QCOMPARE(selected.value("repeatUnit").toString(), QString("monthly"));
        QCOMPARE(selected.value("repeatInterval").toInt(), 2);
        QCOMPARE(selected.value("time").toString(), QString("5:00 PM"));

        // No date means nothing to repeat.
        QVERIFY(backend.setDate(id, ""));
        QVERIFY(!store.reminder(id)->due.isValid());
        QCOMPARE(store.reminder(id)->repeat, QString());

        // A repeat needs a date, and a time needs a day.
        backend.setRepeat(id, "daily", 1);
        QCOMPARE(store.reminder(id)->due, friday.date());
        backend.clearDue(id);
        QVERIFY(backend.setTime(id, "9"));
        QCOMPARE(store.reminder(id)->due, QDate(2026, 11, 14)); // nine has passed today
    }

    void searchAndTheSidebarFindTheirWay() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return friday; });
        const qint64 home = addList(store, "Home");
        add(store, home, "Send the report", friday.date());
        add(store, home, "Buy milk");
        backend.setViewKey("list:" + QString::number(home));

        backend.setSearch("report");
        QCOMPARE(backend.viewKind(), QString("search"));
        QCOMPARE(backend.openCount(), 1);
        backend.setSearch("");
        QCOMPARE(backend.viewKey(), "list:" + QString::number(home));

        // The sidebar counts what is open, and steps through its entries.
        const QVector<SidebarEntry> &entries = backend.views()->entries();
        QCOMPARE(entries.at(0).key, QString("today"));
        QCOMPARE(entries.at(0).count, 1);
        QCOMPARE(entries.at(2).count, 2);
        QCOMPARE(entries.last().name, QString("Home"));
        backend.setViewKey("today");
        backend.stepView(1);
        QCOMPARE(backend.viewKey(), QString("scheduled"));
        backend.stepView(-5);
        QCOMPARE(backend.viewKey(), QString("today"));
    }

    void deletingTheListOnScreenMovesOn() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        add(store, home, "One");
        backend.setViewKey("list:" + QString::number(home));
        backend.removeList(home);
        QCOMPARE(backend.viewKey(), "list:" + QString::number(work));
        backend.undo();
        QCOMPARE(store.lists().size(), 2);
        QCOMPARE(store.reminders().size(), 1);

        backend.setViewKey("list:" + QString::number(home));
        backend.toggleCompleted(store.reminders().constBegin()->id);
        QCOMPARE(backend.completedCount(), 1);
        backend.clearCompleted();
        QCOMPARE(store.reminders().size(), 0);
        backend.undo();
        QCOMPARE(store.reminders().size(), 1);
    }

    void unfinishedDraftsAreClearedAtStartup() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        age(store, add(store, listId, ""), 3600); // left by a window killed an hour ago
        add(store, listId, "Real");
        Backend backend(&store);
        QCOMPARE(store.reminders().size(), 1);
        QVERIFY(!store.canUndo());
    }

    // ---- backend (review fixes) ----

private:
    // Keeps a test's window away from the real systemd: there is no unit to ask about.
    static void quiet(Backend &backend, const QTemporaryDir &dir) {
        backend.setService(dir.filePath("systemctl"), dir.filePath("missing.service"));
    }

    // Runs SQL on the store's file from another connection, as another
    // process would, and has the store read the file again.
    static void behindTheStoresBack(Store &store, const QString &sql, const QVariantList &values = {}) {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "elsewhere");
            db.setDatabaseName(store.path());
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.prepare(sql));
            for (const QVariant &value : values)
                query.addBindValue(value);
            QVERIFY(query.exec());
        }
        QSqlDatabase::removeDatabase("elsewhere");
        store.reloadIfChanged();
    }

    // Makes a reminder look as if it was last touched `seconds` ago.
    static void age(Store &store, qint64 id, qint64 seconds) {
        const qint64 then = QDateTime::currentSecsSinceEpoch() - seconds;
        behindTheStoresBack(store, "UPDATE reminders SET created_at = ?, modified_at = ? WHERE id = ?",
                            {then, then, id});
    }

    static QMetaMethod notifySignal(const QObject &object, const char *property) {
        const QMetaObject *meta = object.metaObject();
        return meta->property(meta->indexOfProperty(property)).notifySignal();
    }

    static bool showsRow(const QVector<Row> &rows, qint64 id) {
        return std::any_of(rows.begin(), rows.end(), [id](const Row &row) { return !row.heading && row.id == id; });
    }

private slots:
    void sectionsAndSortFollowTheView() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        Section section;
        section.listId = home;
        section.name = "Later";
        const qint64 later = store.put(section);
        const qint64 report = add(store, work, "Send the report");
        backend.setViewKey("today");

        // What Move to Section offers follows the view as it switches.
        QSignalSpy sections(&backend, notifySignal(backend, "sections"));
        backend.setViewKey("list:" + QString::number(home));
        QCOMPARE(backend.sections().size(), 1);
        QVERIFY(!sections.isEmpty());
        sections.clear();
        backend.setViewKey("list:" + QString::number(work));
        QVERIFY(backend.sections().isEmpty());
        QVERIFY(!sections.isEmpty());

        // And another list's section is no place for a reminder.
        backend.moveToSection(report, later);
        QCOMPARE(store.reminder(report)->sectionId, 0);
        QVERIFY(store.sections().size() == 1 && store.sections().first().listId == home);

        // Undoing a sort says so, as making it did.
        backend.setViewKey("list:" + QString::number(home));
        QSignalSpy sort(&backend, notifySignal(backend, "sortMode"));
        backend.cycleSort();
        QCOMPARE(backend.sortMode(), QString("due"));
        QVERIFY(!sort.isEmpty());
        sort.clear();
        backend.undo();
        QCOMPARE(backend.sortMode(), QString("manual"));
        QVERIFY(!sort.isEmpty());
        sort.clear();
        backend.redo();
        QCOMPARE(backend.sortMode(), QString("due"));
        QVERIFY(!sort.isEmpty());
    }

    void namingADraftAfterTheSelectionMovedIsStillOneStep() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 first = add(store, listId, "First");
        backend.setViewKey("list:" + QString::number(listId));
        backend.setSelectedId(first);

        // A click on another row moves the selection before the title is in.
        const qint64 id = backend.addReminder();
        backend.setSelectedId(first);
        backend.commitTitle(id, "Buy milk");
        QVERIFY(store.reminder(id));
        QCOMPARE(store.reminder(id)->title, QString("Buy milk"));
        backend.undo();
        QVERIFY(!store.reminder(id));
        QCOMPARE(store.reminders().size(), 1);
    }

    void aDraftLeftEmptyAfterTheSelectionMovedLeavesNoTrace() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 first = add(store, listId, "First");
        const qint64 last = add(store, listId, "Last");
        backend.setViewKey("list:" + QString::number(listId));
        backend.setSelectedId(first);
        store.clearUndo();

        const qint64 id = backend.addReminder();
        backend.setSelectedId(last);
        backend.commitTitle(id, "");
        QVERIFY(!store.reminder(id));
        QVERIFY(!store.canUndo());
        QVERIFY(!store.canRedo());
        QCOMPARE(backend.selectedId(), last); // where the click put it
    }

    void changesToADraftUndoWithIt() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        backend.setClock([] { return friday; });
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        backend.setViewKey("list:" + QString::number(listId));
        backend.setSelectedId(trip);
        store.clearUndo();

        // Tab while typing makes it a subtask; the details pane gives it a day.
        const qint64 id = backend.addReminder();
        backend.indent(id);
        backend.setDueIn(id, 1);
        backend.commitTitle(id, "Book the cabin");
        QCOMPARE(store.reminder(id)->parentId, trip);
        QCOMPARE(store.reminder(id)->due, friday.date().addDays(1));
        backend.undo();
        QVERIFY(!store.reminder(id));
        QVERIFY(!store.canUndo());
    }

    void anotherWindowsDraftSurvivesItsStartup() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store mine(path);
        Backend window(&mine);
        quiet(window, dir);
        window.setViewKey("list:" + QString::number(addList(mine, "Home")));
        const qint64 draft = window.addReminder();

        // A second window opens while the first is still being typed in.
        Store theirs(path);
        Backend second(&theirs);
        quiet(second, dir);
        QVERIFY(theirs.reminder(draft));

        mine.reloadIfChanged();
        window.commitTitle(draft, "Buy milk");
        QVERIFY(mine.reminder(draft));
        QCOMPARE(mine.reminder(draft)->title, QString("Buy milk"));
    }

    void deletingCompletedTakesOnlyWhatTheViewHolds() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        backend.setClock([] { return friday; });
        const qint64 home = addList(store, "Home"), work = addList(store, "Work");
        Reminder tagged;
        tagged.listId = work;
        tagged.title = "Filed the report";
        tagged.tags = {"work"};
        const qint64 filed = store.put(tagged);
        const qint64 paid = add(store, home, "Paid the rent", friday.date());
        const qint64 cleaned = add(store, home, "Cleaned the gutters");
        for (qint64 id : {filed, paid, cleaned})
            store.complete(id, friday);
        backend.setShowCompleted(true);

        // In #work only the one done there is on screen, and only it goes.
        backend.setViewKey("tag:work");
        QCOMPARE(backend.reminders()->rowCount(), 1);
        QCOMPARE(backend.completedCount(), 1);
        backend.clearCompleted();
        QVERIFY(!store.reminder(filed));
        QVERIFY(store.reminder(paid) && store.reminder(cleaned));

        backend.setViewKey("today");
        QCOMPARE(backend.completedCount(), 1);
        backend.clearCompleted();
        QVERIFY(!store.reminder(paid));
        QVERIFY(store.reminder(cleaned));

        // A list still clears all it has done, shown or not.
        backend.setShowCompleted(false);
        backend.setViewKey("list:" + QString::number(home));
        QCOMPARE(backend.completedCount(), 1);
        backend.clearCompleted();
        QVERIFY(!store.reminder(cleaned));
    }

    void deletingCompletedLeavesNoSubtaskWithoutItsParent() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 home = addList(store, "Home");
        // A finished reminder with a subtask still open, as an older Nudge could leave it.
        Reminder trip;
        trip.listId = home;
        trip.title = "Trip";
        trip.completed = true;
        trip.completedAt = friday.toSecsSinceEpoch();
        const qint64 tripId = store.put(trip);
        Reminder cabin;
        cabin.listId = home;
        cabin.parentId = tripId;
        cabin.title = "Book the cabin";
        const qint64 cabinId = store.put(cabin);
        // And one done with its subtasks, tagged where they are not.
        Reminder project;
        project.listId = home;
        project.title = "Project";
        project.tags = {"work"};
        const qint64 projectId = store.put(project);
        Reminder step;
        step.listId = home;
        step.parentId = projectId;
        step.title = "First step";
        const qint64 stepId = store.put(step);
        store.complete(projectId, friday);

        auto noneAdrift = [&store] {
            for (const Reminder &r : store.reminders())
                if (r.parentId && !store.reminder(r.parentId))
                    return false;
            return true;
        };
        backend.setViewKey("tag:work");
        QCOMPARE(backend.completedCount(), 2);
        backend.clearCompleted();
        QVERIFY(!store.reminder(projectId) && !store.reminder(stepId));
        QVERIFY(noneAdrift());

        backend.setViewKey("list:" + QString::number(home));
        backend.clearCompleted();
        QVERIFY(noneAdrift());
        QVERIFY(store.reminder(cabinId) && !store.reminder(cabinId)->completed);
    }

    void aChangeThatCannotBeSavedSaysSo() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Dentist");
        const qint64 other = add(store, listId, "Groceries");
        backend.setViewKey("list:" + QString::number(listId));
        // The file starts refusing what is written to it, as a full disk would.
        behindTheStoresBack(store, "CREATE TRIGGER refuse BEFORE INSERT ON reminders "
                                   "BEGIN SELECT RAISE(ABORT, 'database or disk is full'); END");
        behindTheStoresBack(store, "CREATE TRIGGER keep BEFORE DELETE ON reminders "
                                   "BEGIN SELECT RAISE(ABORT, 'database or disk is full'); END");

        QSignalSpy notices(&backend, &Backend::notice);
        backend.setTitle(id, "Dentist at nine");
        QCOMPARE(store.reminder(id)->title, QString("Dentist"));
        QTRY_VERIFY(!notices.isEmpty());
        QVERIFY2(notices.last().first().toString().contains("database or disk is full"),
                 qPrintable(notices.last().first().toString()));

        // A deletion that failed says so, and never claims to have deleted.
        notices.clear();
        backend.remove(other);
        QVERIFY(store.reminder(other));
        QTRY_VERIFY(!notices.isEmpty());
        for (const QList<QVariant> &notice : notices)
            QVERIFY2(!notice.first().toString().startsWith("Deleted"), qPrintable(notice.first().toString()));
        QVERIFY2(notices.last().first().toString().contains("database or disk is full"),
                 qPrintable(notices.last().first().toString()));
    }

    void aTagWrittenTwoWaysIsOneEntry() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        auto tag = [&](const QString &name) {
            Reminder r;
            r.listId = listId;
            r.title = "About " + name;
            r.tags = {name};
            r.position = store.endPosition(listId);
            store.put(r);
        };
        // Written as often each way, the entry is the same whatever order the
        // store holds them in; written more one way, it is that way.
        for (int i = 0; i < 8; ++i) {
            tag(QString("tag%1").arg(i));
            tag(QString("Tag%1").arg(i));
        }
        tag("Errand");
        tag("errand");
        tag("errand");
        Backend backend(&store);
        quiet(backend, dir);

        QStringList shown, expected{"tag:errand errand 3"};
        for (int i = 0; i < 8; ++i)
            expected << QString("tag:Tag%1 Tag%1 2").arg(i);
        for (const SidebarEntry &entry : backend.views()->entries())
            if (entry.group == "tags")
                shown << entry.key + " " + entry.name + " " + QString::number(entry.count);
        QCOMPARE(shown, expected);

        // A view opened under another spelling is the entry's own.
        backend.setViewKey("tag:ERRAND");
        QCOMPARE(backend.viewKey(), QString("tag:errand"));
        QCOMPARE(backend.reminders()->rowCount(), 3);
    }

    void completingAParentLingersOnlyWhatItCompleted() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        auto subtask = [&](const QString &title) {
            Reminder r;
            r.listId = listId;
            r.parentId = trip;
            r.title = title;
            r.position = store.endPosition(listId);
            return store.put(r);
        };
        const qint64 booked = subtask("Book the cabin");
        subtask("Pack the car");
        store.complete(booked, friday.addDays(-30));
        backend.setViewKey("list:" + QString::number(listId));
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"Trip", "  Pack the car"}));

        // What it completes stays a moment; what was done a month ago stays out of sight.
        backend.toggleCompleted(trip);
        QVERIFY(store.reminder(trip)->completed);
        QCOMPARE(titles(store, backend.reminders()->rows()), (QStringList{"Trip", "  Pack the car"}));
        QVERIFY(backend.reminders()->rowOf(booked) < 0);
    }

    void anOpenSubtaskShowsWhenItsParentIsHidden() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        const qint64 listId = addList(store, "Home");
        // A finished reminder with a subtask still open, as an older Nudge could leave it.
        Reminder trip;
        trip.listId = listId;
        trip.title = "Trip";
        trip.completed = true;
        trip.completedAt = friday.toSecsSinceEpoch();
        const qint64 tripId = store.put(trip);
        Reminder cabin;
        cabin.listId = listId;
        cabin.parentId = tripId;
        cabin.title = "Book the cabin";
        cabin.position = 1;
        const qint64 cabinId = store.put(cabin);
        add(store, listId, "Water the plants");

        ViewOptions options;
        options.now = friday;
        for (const View &view : {View{View::List, listId, {}}, View{View::All, 0, {}}}) {
            const QVector<Row> rows = buildRows(store, view, options);
            QVERIFY2(showsRow(rows, cabinId), qPrintable(titles(store, rows).join(", ")));
        }
        if (store.reminder(tripId)->completed) {
            // On its own, where its parent would be, and saying whose it is.
            QCOMPARE(titles(store, buildRows(store, View{View::List, listId, {}}, options)),
                     (QStringList{"Book the cabin", "Water the plants"}));
            ReminderModel model(&store);
            model.show(buildRows(store, View{View::List, listId, {}}, options), true);
            QCOMPARE(model.data(model.index(model.rowOf(cabinId)), ReminderModel::ParentTitleRole).toString(),
                     QString("Trip"));
        }
        options.showCompleted = true;
        QCOMPARE(titles(store, buildRows(store, View{View::List, listId, {}}, options)),
                 (QStringList{"Trip", "  Book the cabin", "Water the plants"}));
    }

    void subtasksAreNotAddedUnderFinishedReminders() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Trip");
        const qint64 cabin = add(store, listId, "Book the cabin");
        const qint64 car = add(store, listId, "Pack the car");
        backend.setViewKey("list:" + QString::number(listId));
        backend.indent(cabin);
        store.complete(trip, friday);
        backend.setShowCompleted(true);
        store.clearUndo();

        // Tab under a finished reminder means it is not finished after all.
        QSignalSpy notices(&backend, &Backend::notice);
        backend.indent(car);
        QCOMPARE(store.reminder(car)->parentId, trip);
        QVERIFY(!store.reminder(trip)->completed);
        QCOMPARE(notices.count(), 1);
        backend.setShowCompleted(false);
        QVERIFY(backend.reminders()->rowOf(car) >= 0);
        // One step takes both back.
        backend.undo();
        QCOMPARE(store.reminder(car)->parentId, 0);
        QVERIFY(store.reminder(trip)->completed);

        // N beside a finished subtask adds a reminder of its own, below what it belongs to.
        backend.setShowCompleted(true);
        backend.setSelectedId(cabin);
        const qint64 id = backend.addReminder();
        QCOMPARE(store.reminder(id)->parentId, 0);
        backend.commitTitle(id, "Buy snacks");
        QCOMPARE(titles(store, backend.reminders()->rows()),
                 (QStringList{"Trip", "  Book the cabin", "Buy snacks", "Pack the car"}));
        QVERIFY(store.reminder(trip)->completed);
    }

    void linksSayWhatKindTheyAre() {
        QCOMPARE(ReminderModel::linkLabel("https://www.example.com/a?b=c"), QString("example.com"));
        QCOMPARE(ReminderModel::linkLabel("example.com/path"), QString("example.com"));
        QCOMPARE(ReminderModel::linkLabel("http://docs.example.com"), QString("docs.example.com"));
        // Anything but a web page says what kind of link it is.
        QCOMPARE(ReminderModel::linkLabel("claude://claude.ai/new?q=hello"), QString("claude://claude.ai"));
        QCOMPARE(ReminderModel::linkLabel("zoommtg://zoom.us/join?confno=1"), QString("zoommtg://zoom.us"));
        QCOMPARE(ReminderModel::linkLabel("vnd.sun.star.webdavs://docs.example.com/Q3.odt"),
                 QString("vnd.sun.star.webdavs://docs.example.com"));
        QCOMPARE(ReminderModel::linkLabel("file:///home/me/notes.txt"), QString("file:///home/me/notes.txt"));
        QCOMPARE(ReminderModel::linkLabel("  "), QString());
    }

    void backgroundRemindersGoThroughSystemd() {
        QTemporaryDir dir;
        const QString log = dir.filePath("calls"), state = dir.filePath("state"), systemctl = dir.filePath("systemctl");
        QFile script(systemctl);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(("#!/bin/sh\necho \"$@\" >> '" + log + "'\ncase \"$2\" in\n  enable) echo enabled > '" + state +
                      "' ;;\n  disable) echo disabled > '" + state + "' ;;\n  is-enabled) cat '" + state +
                      "' 2>/dev/null || echo disabled ;;\nesac\n")
                         .toUtf8());
        script.close();
        QVERIFY(QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        // Without the package's unit there is nothing to turn on, and it says so.
        backend.setService(systemctl, dir.filePath("missing.service"));
        QVERIFY(!backend.backgroundAvailable());
        QSignalSpy notices(&backend, &Backend::notice);
        backend.setBackground(true);
        QCOMPARE(notices.count(), 1);
        QVERIFY(!backend.backgroundOn());

        QFile unit(dir.filePath("nudge.service"));
        QVERIFY(unit.open(QIODevice::WriteOnly));
        unit.close();
        backend.setService(systemctl, unit.fileName());
        QVERIFY(backend.backgroundAsk());
        backend.setBackground(true);
        QVERIFY(backend.backgroundOn());
        QVERIFY(!backend.backgroundAsk());
        backend.setBackground(false);
        QVERIFY(!backend.backgroundOn());
    }

    // ---- cli (review fixes) ----

private:
    struct CliRun {
        int code = -1;
        QString out, err;
        QStringList warnings;
    };

    // Runs `nudge <arguments>` on the file at `database`, as the command line
    // would, and keeps what it printed and warned. NUDGE_DB is set only for
    // the run, and nothing runs unless the store would open that file, so a
    // command never reaches the real reminders.
    static CliRun cliRun(const QString &database, const QStringList &arguments) {
        CliRun ran;
        const bool hadDatabase = qEnvironmentVariableIsSet("NUDGE_DB");
        const QByteArray previous = qgetenv("NUDGE_DB");
        qputenv("NUDGE_DB", QFile::encodeName(database));
        QTemporaryFile out, err;
        if (Store::defaultPath() == database && out.open() && err.open()) {
            static QStringList *warnings = nullptr;
            warnings = &ran.warnings;
            const QtMessageHandler before = qInstallMessageHandler(
                [](QtMsgType, const QMessageLogContext &, const QString &message) { *warnings << message; });
            fflush(stdout);
            fflush(stderr);
            const int savedOut = ::dup(STDOUT_FILENO), savedErr = ::dup(STDERR_FILENO);
            ::dup2(out.handle(), STDOUT_FILENO);
            ::dup2(err.handle(), STDERR_FILENO);
            ran.code = runCli(QStringList{"nudge"} + arguments);
            fflush(stdout);
            fflush(stderr);
            ::dup2(savedOut, STDOUT_FILENO);
            ::dup2(savedErr, STDERR_FILENO);
            ::close(savedOut);
            ::close(savedErr);
            qInstallMessageHandler(before);
            out.seek(0);
            err.seek(0);
            ran.out = QString::fromUtf8(out.readAll());
            ran.err = QString::fromUtf8(err.readAll());
        }
        if (hadDatabase)
            qputenv("NUDGE_DB", previous);
        else
            qunsetenv("NUDGE_DB");
        return ran;
    }

    static QJsonObject cliJson(const CliRun &ran) { return QJsonDocument::fromJson(ran.out.toUtf8()).object(); }

private slots:
    void commandOutputCannotSteerTheTerminal() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        // An import or another program can leave anything in a field: here a
        // clipboard write, a link, a title change, cursor moves, line breaks,
        // and bidi and C1 controls.
        const qint64 listId = addList(store, "Inbox\x1b]52;c;aGk=\x07\nEvil");
        Reminder r;
        r.listId = listId;
        r.title = QString("Pay\x1b]8;;http://x\x07 rent\r\x1b[1A\x1b[2Kfake") + QChar(0x202e) + QChar(0x2066) +
                  QChar(0x85) + QChar(0x9b) + QChar(0x7f) + QChar(0x2028);
        r.notes = "line1\r\nline2\x1b]0;title\x07";
        r.url = "https://example.com/\ninjected";
        r.tags = {"home\x1b[31m"};
        r.due = QDate(2030, 5, 1);
        const qint64 id = store.put(r);
        Reminder subtask;
        subtask.listId = listId;
        subtask.parentId = id;
        subtask.title = "Sub\nline";
        subtask.position = 1;
        store.put(subtask);
        const QString number = QString::number(id);

        // The first character that could steer a terminal, by its number, so
        // a failure does not send it to the terminal running the tests.
        auto steering = [](const QString &text) {
            static const QRegularExpression controls(
                "[\\x{0}-\\x{8}\\x{b}-\\x{1f}\\x{7f}-\\x{9f}\\x{2028}\\x{2029}\\x{202a}-\\x{202e}\\x{2066}-\\x{2069}]");
            const QRegularExpressionMatch found = controls.match(text);
            return found.hasMatch() ? QString("U+%1").arg(int(found.captured().at(0).unicode()), 4, 16, QChar('0'))
                                    : QString();
        };
        auto lines = [](const QString &text) { return text.split('\n', Qt::SkipEmptyParts).size(); };

        CliRun ran = cliRun(path, {"list", "all"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(steering(ran.out), QString());
        QCOMPARE(lines(ran.out), 2); // a line a reminder, whatever its fields hold
        QVERIFY(ran.out.contains("Sub\\nline"));
        QVERIFY(ran.out.contains("rent\\r"));
        QVERIFY(ran.out.contains(QChar::ReplacementCharacter));

        ran = cliRun(path, {"show", number});
        QCOMPARE(ran.code, 0);
        QCOMPARE(steering(ran.out), QString());
        QCOMPARE(lines(ran.out), 5); // the reminder, two lines of notes, the link, the subtask
        QVERIFY(ran.out.contains("https://example.com/\\ninjected"));

        ran = cliRun(path, {"lists"});
        QCOMPARE(steering(ran.out), QString());
        QCOMPARE(lines(ran.out), 1);

        // An error that names the lists goes to stderr just as safely.
        ran = cliRun(path, {"list", "--list", "Nowhere"});
        QCOMPARE(ran.code, 1);
        QCOMPARE(steering(ran.err), QString());
        QCOMPARE(lines(ran.err), 1);

        ran = cliRun(path, {"edit", number, "--priority", "high"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(steering(ran.out), QString());
        QCOMPARE(lines(ran.out), 1);

        ran = cliRun(path, {"done", number});
        QCOMPARE(ran.code, 0);
        QCOMPARE(steering(ran.out), QString());
        QCOMPARE(lines(ran.out), 1);

        // JSON escapes what a field holds and keeps all of it.
        ran = cliRun(path, {"show", number, "--json"});
        QCOMPARE(cliJson(ran).value("title").toString(), r.title);
        QCOMPARE(cliJson(ran).value("url").toString(), r.url);
    }

    void commandTimesAndDatesFollowTheWindow() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        // A time without a day is the next time the clock shows it, and
        // midnight has always just gone.
        const QDate before = QDate::currentDate();
        CliRun ran = cliRun(path, {"add", "Take pills", "--time", "00:00", "--json"});
        const QDate after = QDate::currentDate();
        QCOMPARE(ran.code, 0);
        const QDate due = QDate::fromString(cliJson(ran).value("due").toString(), Qt::ISODate);
        QVERIFY2(due > before && due <= after.addDays(1), qPrintable(due.toString(Qt::ISODate)));
        QCOMPARE(cliJson(ran).value("time").toString(), QString("00:00"));

        // A date says which day a monthly repeat returns to, as the window has it.
        ran = cliRun(path, {"add", "Pay rent every month", "--due", "2030-01-31", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("monthly:1:31"));
        const QString rent = QString::number(cliJson(ran).value("id").toInteger());
        ran = cliRun(path, {"edit", rent, "--due", "2030-03-15", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2030-03-15"));
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("monthly:1:15"));
    }

    void commandRepeatsKeepTheirDay() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        // "every monday" moves a Wednesday to the Monday after it, as quick
        // entry does, so the series falls on Mondays.
        CliRun ran = cliRun(path, {"add", "Gym", "--due", "2030-05-01", "--repeat", "every monday", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2030-05-06"));
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("weekly:1"));

        // Without a date it starts on the coming Monday, today if it is one.
        const QDate before = QDate::currentDate();
        ran = cliRun(path, {"add", "Swim", "--repeat", "every other monday", "--json"});
        const QDate after = QDate::currentDate();
        QCOMPARE(ran.code, 0);
        const QDate due = QDate::fromString(cliJson(ran).value("due").toString(), Qt::ISODate);
        QCOMPARE(due.dayOfWeek(), 1);
        QVERIFY2(due >= before && due <= after.addDays(6), qPrintable(due.toString(Qt::ISODate)));
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("weekly:2"));

        // A series on weekdays starts on one.
        ran = cliRun(path, {"add", "Commute", "--due", "2030-05-04", "--repeat", "weekdays", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2030-05-06"));

        // A repeat that names no day keeps the date it has.
        ran = cliRun(path, {"add", "Bins", "--due", "2030-05-01", "--repeat", "every 2 weeks", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2030-05-01"));
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("weekly:2"));
    }

    void clearingADateOrRepeatClearsWhereItEnds() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = "Water the plants";
        r.due = QDate(2030, 5, 1);
        r.dueMinutes = 9 * 60;
        r.repeat = "weekly:1";
        r.repeatEnd = QDate(2030, 6, 1);
        r.early = 30;
        const QString water = QString::number(store.put(r));
        r.title = "Feed the cat";
        const QString feed = QString::number(store.put(r));

        // Without a repeat there is no end to it, or a later repeat would
        // stop at the old one.
        CliRun ran = cliRun(path, {"edit", water, "--repeat", "none", "--json"});
        QCOMPARE(ran.code, 0);
        QVERIFY(cliJson(ran).value("repeat").isNull());
        QVERIFY(cliJson(ran).value("repeatEnd").isNull());
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2030-05-01"));

        // Without a date there is nothing to repeat or to alert ahead of.
        ran = cliRun(path, {"edit", feed, "--due", "none", "--json"});
        QCOMPARE(ran.code, 0);
        QVERIFY(cliJson(ran).value("due").isNull());
        QVERIFY(cliJson(ran).value("time").isNull());
        QVERIFY(cliJson(ran).value("repeat").isNull());
        QVERIFY(cliJson(ran).value("repeatEnd").isNull());
        QCOMPARE(cliJson(ran).value("early").toInt(), 0);
    }

    void commandTagsAreSingleWords() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        // The store keeps tags apart by spaces, so a tag cannot hold one.
        CliRun ran = cliRun(path, {"add", "Clear the desk", "--tag", "home office", "--tag", "#work,", "--json"});
        QCOMPARE(ran.code, 0);
        const QStringList tags{"home", "office", "work"};
        QCOMPARE(cliJson(ran).value("tags").toVariant().toStringList(), tags);
        const qint64 id = cliJson(ran).value("id").toInteger();
        // What was printed is what is read back.
        QCOMPARE(Store(path).reminder(id)->tags, tags);

        ran = cliRun(path, {"edit", QString::number(id), "--tags", "#a,b\tc  A", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("tags").toVariant().toStringList(), (QStringList{"a", "b", "c"}));
        QCOMPARE(Store(path).reminder(id)->tags, (QStringList{"a", "b", "c"}));
    }

    void commandsActOnARepeatedIdOnce() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = "Stretch";
        r.due = QDate(2030, 5, 1);
        r.repeat = "daily:1";
        const qint64 id = store.put(r);
        const QString number = QString::number(id);

        // Named twice, it is still done once: one day on, one completion.
        CliRun ran = cliRun(path, {"done", number, number, "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("reminders").toArray().size(), 1);
        Store check(path);
        QCOMPARE(check.reminder(id)->due, QDate(2030, 5, 2));
        QCOMPARE(check.reminders().size(), 2); // it, and the record of one completion

        ran = cliRun(path, {"delete", number, number});
        QCOMPARE(ran.code, 0);
        QCOMPARE(ran.out, "Deleted " + number + ": Stretch\n");
    }

    void importingLeavesTheListsHereAlone() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        TaskList home;
        home.name = "Home";
        home.color = "green";
        home.position = 1;
        const qint64 homeId = store.put(home);
        const QString file = dir.filePath("export.json");
        write(file, R"({"nudge": 1, "reminders": [], "lists": [
            {"name": "home", "color": "red", "sort": "due", "sections": []},
            {"name": "Garden", "color": "cyan", "sort": "title", "sections": ["Beds"]}]})");

        const CliRun ran = cliRun(path, {"import", file});
        QCOMPARE(ran.code, 0);
        Store check(path);
        QCOMPARE(check.list(homeId)->color, QString("green"));
        QCOMPARE(check.list(homeId)->sort, QString("manual"));
        // A list the import makes takes the look it had.
        QCOMPARE(check.lists().size(), 2);
        const TaskList garden = check.lists().last();
        QCOMPARE(garden.name, QString("Garden"));
        QCOMPARE(garden.color, QString("cyan"));
        QCOMPARE(garden.sort, QString("title"));
    }

    void aSubtaskStaysInItsParentsList() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        const qint64 home = addList(store, "Home");
        addList(store, "Work");
        const QString parent = QString::number(add(store, home, "Plan the trip"));

        CliRun ran = cliRun(path, {"add", "Book a cabin", "--parent", parent, "--list", "Work"});
        QCOMPARE(ran.code, 1);
        QCOMPARE(ran.err, QString("A subtask lives in its parent's list; leave out --list.\n"));
        QCOMPARE(Store(path).reminders().size(), 1);

        // Naming the parent's own list moves nothing, so it is fine.
        ran = cliRun(path, {"add", "Book a cabin", "--parent", parent, "--list", "home", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(QString::number(cliJson(ran).value("parent").toInteger()), parent);
        QCOMPARE(cliJson(ran).value("list").toString(), QString("Home"));
    }

    void exportAndSkillTakeJson() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        add(store, addList(store, "Home"), "Water the plants");

        const QString file = dir.filePath("backup.json");
        CliRun ran = cliRun(path, {"export", file, "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("exported").toInt(), 1);
        QCOMPARE(cliJson(ran).value("file").toString(), QFileInfo(file).absoluteFilePath());
        QVERIFY(QFile::exists(file));

        // To standard output an export is JSON either way.
        ran = cliRun(path, {"export", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("reminders").toArray().size(), 1);

        ran = cliRun(path, {"skill", "--json"});
        QCOMPARE(ran.code, 0);
        QVERIFY(cliJson(ran).value("skill").toString().startsWith("---\nname: nudge\n"));
    }

    void addingAReminderWarnsOfNothing() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        CliRun ran = cliRun(path, {"add", "Water the plants", "--flag", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(ran.warnings, QStringList());
        QVERIFY(cliJson(ran).value("flagged").toBool());

        // Taking the flag off is edit's.
        ran = cliRun(path, {"edit", QString::number(cliJson(ran).value("id").toInteger()), "--unflag", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(ran.warnings, QStringList());
        QVERIFY(!cliJson(ran).value("flagged").toBool());
    }

    void namesWithPercentSignsPrintAsTheyAre() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        add(store, addList(store, "Save 50%2 now"), "Coupons");
        const CliRun ran = cliRun(path, {"lists", "delete", "Save 50%2 now"});
        QCOMPARE(ran.code, 1);
        QCOMPARE(ran.err, QString("Save 50%2 now holds 1 reminders. Add --force to delete them with it.\n"));
    }

    void aSubtaskForADoneReminderReopensIt() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        const qint64 listId = addList(store, "Home");
        const qint64 trip = add(store, listId, "Plan the trip");
        store.complete(trip, QDateTime::currentDateTime());

        // A done reminder has no open subtasks, so adding one reopens it,
        // and says so, rather than hide the new one away.
        CliRun ran = cliRun(path, {"add", "Book a cabin", "--parent", QString::number(trip)});
        QCOMPARE(ran.code, 0);
        QVERIFY2(ran.out.contains("Reopened " + QString::number(trip) + ": Plan the trip"), qPrintable(ran.out));
        QVERIFY(!Store(path).reminder(trip)->completed);
        ran = cliRun(path, {"list", "all"});
        QCOMPARE(ran.out.split('\n', Qt::SkipEmptyParts).size(), 2);

        // Nor can an import leave an open subtask under a done reminder.
        const QString file = dir.filePath("export.json");
        write(file, R"({"nudge": 1, "lists": [], "reminders": [
            {"id": 1, "title": "Pack", "list": "Home", "completed": true, "completedAt": "2026-11-01T10:00:00"},
            {"id": 2, "parent": 1, "title": "Buy rope", "list": "Home"}]})");
        ran = cliRun(path, {"import", file});
        QCOMPARE(ran.code, 0);
        Store check(path);
        for (const Reminder &r : check.reminders())
            if (r.title == "Pack")
                QVERIFY(!r.completed);
        ran = cliRun(path, {"list", "all"});
        QCOMPARE(ran.out.split('\n', Qt::SkipEmptyParts).size(), 4);
    }

    // Worked out only: nothing is started.
    void aClickedBackgroundAlertOpensARealWindow() {
        // The service draws offscreen, as these tests do; the window it opens
        // gets the session's own environment back, which named no platform.
        QProcessEnvironment session;
        session.insert("WAYLAND_DISPLAY", "wayland-1");
        const WindowLaunch scoped = windowLaunch("/usr/bin/nudge", session, "/usr/bin/systemd-run");
        QCOMPARE(scoped.program, QString("/usr/bin/systemd-run"));
        // A scope of its own, so turning the service off leaves the window open.
        QCOMPARE(scoped.arguments, (QStringList{"--user", "--scope", "--collect", "--quiet", "/usr/bin/nudge", "open"}));
        QVERIFY(scoped.environment == session);
        QVERIFY(!scoped.environment.contains("QT_QPA_PLATFORM"));

        // Without systemd-run it starts directly, still as the session had it.
        session.insert("QT_QPA_PLATFORM", "wayland");
        const WindowLaunch direct = windowLaunch("/usr/bin/nudge", session, QString());
        QCOMPARE(direct.program, QString("/usr/bin/nudge"));
        QCOMPARE(direct.arguments, QStringList{"open"});
        QCOMPARE(direct.environment.value("QT_QPA_PLATFORM"), QString("wayland"));
    }

    // ---- Alerts -------------------------------------------------------------------

    void commandsSayWhenAWriteFails() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        qint64 id = 0;
        {
            Store store(path);
            id = add(store, addList(store, "Home"), "Milk");
        }
        // Every reminder write is an INSERT OR REPLACE, so this refuses them all.
        tamper(path, "CREATE TRIGGER refuse BEFORE INSERT ON reminders BEGIN SELECT RAISE(ABORT, 'disk is full'); END");
        const QString number = QString::number(id);

        const CliRun edited = cliRun(path, {"edit", number, "--title", "Oat milk"});
        QCOMPARE(edited.code, 1);
        QVERIFY2(!edited.out.contains("Changed"), qPrintable(edited.out));
        QVERIFY2(edited.err.contains("disk is full"), qPrintable(edited.err));

        const CliRun done = cliRun(path, {"done", number});
        QCOMPARE(done.code, 1);
        QVERIFY2(!done.out.contains("Completed"), qPrintable(done.out));
        QVERIFY2(done.err.contains("disk is full"), qPrintable(done.err));

        const CliRun added = cliRun(path, {"add", "--literal", "Bread"});
        QCOMPARE(added.code, 1);
        QVERIFY2(!added.out.contains("Added"), qPrintable(added.out));

        QFile backup(dir.filePath("backup.json"));
        QVERIFY(backup.open(QIODevice::WriteOnly));
        backup.write(R"({"nudge": 1, "lists": [{"name": "Garden"}],
                         "reminders": [{"id": 1, "title": "Plant bulbs", "list": "Garden"}]})");
        backup.close();
        const CliRun imported = cliRun(path, {"import", backup.fileName()});
        QCOMPARE(imported.code, 1);
        QVERIFY2(!imported.out.contains("Imported"), qPrintable(imported.out));
        QVERIFY2(imported.err.contains("Nothing was imported"), qPrintable(imported.err));

        // Nothing reached the file: not the edits, not the import's new list.
        Store store(path);
        QCOMPARE(store.reminders().size(), 1);
        QCOMPARE(store.reminder(id)->title, QString("Milk"));
        QVERIFY(!store.reminder(id)->completed);
        QCOMPARE(store.lists().size(), 1);
    }

    void remindersAlertOnceWhenDue() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Call the dentist", QDate(2030, 5, 1), 9 * 60);
        const QDateTime due(QDate(2030, 5, 1), QTime(9, 0));

        notifier.check(due.addSecs(-60));
        QCOMPARE(alerts.sent.size(), 0);
        notifier.check(due);
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(alerts.sent.first().title, QString("Call the dentist"));
        QVERIFY(alerts.sent.first().body.contains("Home"));
        QVERIFY(alerts.sent.first().actions.contains("done"));
        notifier.check(due.addSecs(120));
        QCOMPARE(alerts.sent.size(), 1);

        // Completing from the alert completes the reminder.
        notifier.act(1, "done", due.addSecs(180));
        QVERIFY(store.reminder(id)->completed);
    }

    void anAllDayReminderAlertsInTheMorning() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        add(store, addList(store, "Home"), "Renew passport", QDate(2030, 5, 1));
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(8, 59)));
        QCOMPARE(alerts.sent.size(), 0);
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(9, 0)));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void earlyRemindersAlertAhead() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = "Flight";
        r.due = QDate(2030, 5, 1);
        r.dueMinutes = 9 * 60;
        r.early = 30;
        store.put(r);
        const QDateTime due(QDate(2030, 5, 1), QTime(9, 0));

        notifier.check(due.addSecs(-31 * 60));
        QCOMPARE(alerts.sent.size(), 0);
        notifier.check(due.addSecs(-30 * 60));
        QCOMPARE(alerts.sent.size(), 1);
        QVERIFY(alerts.sent.last().body.startsWith("Coming up"));
        notifier.check(due);
        QCOMPARE(alerts.sent.size(), 2);
    }

    void aDateSetAfterItsMomentDoesNotAlert() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        // Set just now, for a time long past: old news, not an alert.
        add(store, addList(store, "Home"), "Should have called", QDate(2020, 1, 1), 9 * 60);
        notifier.check(QDateTime::currentDateTime());
        QCOMPARE(alerts.sent.size(), 0);
    }

    void snoozingBringsTheAlertBack() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 id = add(store, addList(store, "Home"), "Stretch", QDate(2030, 5, 1), 9 * 60);
        const QDateTime due(QDate(2030, 5, 1), QTime(9, 0));
        notifier.check(due);
        notifier.act(1, "hour", due);
        QVERIFY(!store.reminder(id)->completed);
        notifier.check(due.addSecs(3599));
        QCOMPARE(alerts.sent.size(), 1);
        notifier.check(due.addSecs(3600));
        QCOMPARE(alerts.sent.size(), 2);
        notifier.check(due.addSecs(7200));
        QCOMPARE(alerts.sent.size(), 2);
    }

    void aPileOfAlertsArrivesAsOne() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 listId = addList(store, "Home");
        for (int i = 0; i < 6; ++i)
            add(store, listId, QString("Thing %1").arg(i), QDate(2030, 5, 1), 9 * 60);
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(12, 0)));
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(alerts.sent.first().title, QString("6 reminders are due"));
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(12, 5)));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void wakingFromSleepChecksAtOnce() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        const QDateTime now = QDateTime::currentDateTime();
        add(store, addList(store, "Home"), "Due while the lid was closed", now.date(),
            now.time().hour() * 60 + now.time().minute());
        // Its timer would not have run during the sleep; logind's signal does the waking.
        Notifier notifier(&store, &alerts);
        QVERIFY(QMetaObject::invokeMethod(&notifier, "onPrepareForSleep", Q_ARG(bool, true)));
        QCOMPARE(alerts.sent.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(&notifier, "onPrepareForSleep", Q_ARG(bool, false)));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void wakingOnANewDayTurnsToday() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        QDateTime clock = friday;
        backend.setClock([&clock] { return clock; });
        add(store, addList(store, "Home"), "Saturday errand", friday.date().addDays(1));
        backend.setViewKey("today");
        QCOMPARE(backend.reminders()->rowCount(), 0);

        clock = friday.addDays(1);
        QVERIFY(QMetaObject::invokeMethod(&backend, "onPrepareForSleep", Q_ARG(bool, false)));
        QCOMPARE(backend.reminders()->rowCount(), 1);
        QCOMPARE(backend.views()->entries().first().count, 1);
    }

    void onlyOneProcessAlerts() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        alerts.allowed = false; // the background service has the job
        Notifier notifier(&store, &alerts);
        const qint64 id = add(store, addList(store, "Home"), "Stretch", QDate(2030, 5, 1), 9 * 60);
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(9, 0)));
        QCOMPARE(alerts.sent.size(), 0);
        QCOMPARE(store.reminder(id)->alertedDue, 0); // left for the one that does alert
    }

    // ---- alerts (review fixes) ----

private:
    // A reminder that comes back every day at the time of `at`.
    static qint64 addDaily(Store &store, const QString &title, const QDateTime &at) {
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = title;
        r.due = at.date();
        r.dueMinutes = at.time().hour() * 60 + at.time().minute();
        r.repeat = "daily:1";
        return store.put(r);
    }

    static int completedCopies(const Store &store, const QString &title) {
        int count = 0;
        for (const Reminder &r : store.reminders())
            count += r.completed && r.title == title;
        return count;
    }

private slots:
    // An alert left on screen answers for the moment it announced. Completed
    // in the window since, a daily reminder has moved on to tomorrow, and a
    // late Complete on the alert must not complete tomorrow's as well.
    void aLateCompleteLeavesTheNextRepeatAlone() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const QDateTime eight(QDate(2030, 5, 1), QTime(8, 0));
        const qint64 id = addDaily(store, "Take pills", eight);
        notifier.check(eight);
        QCOMPARE(alerts.sent.size(), 1);

        store.complete(id, eight.addSecs(5 * 60)); // in the window
        QCOMPARE(store.reminder(id)->due, QDate(2030, 5, 2));
        notifier.act(1, "done", eight.addSecs(10 * 60));
        QCOMPARE(store.reminder(id)->due, QDate(2030, 5, 2));
        QCOMPARE(completedCopies(store, "Take pills"), 1);
    }

    // Nor does a late In an Hour or Tomorrow snooze tomorrow's.
    void aLateSnoozeLeavesTheNextRepeatAlone() {
        for (const QString action : {"hour", "tomorrow"}) {
            QTemporaryDir dir;
            Store store(dir.filePath("nudge.db"));
            FakeAlerts alerts;
            Notifier notifier(&store, &alerts);
            const QDateTime eight(QDate(2030, 5, 1), QTime(8, 0));
            const qint64 id = addDaily(store, "Take pills", eight);
            notifier.check(eight);
            store.complete(id, eight.addSecs(5 * 60));

            notifier.act(1, action, eight.addSecs(10 * 60));
            QCOMPARE(store.reminder(id)->snoozeUntil, 0);
            notifier.check(QDateTime(QDate(2030, 5, 2), QTime(7, 59)));
            QCOMPARE(alerts.sent.size(), 1);
        }
    }

    // The same goes for a reminder given another time since, or deleted;
    // the alert for its new time answers as before.
    void aLateClickOnAMovedOrDeletedReminderDoesNothing() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Call the dentist", QDate(2030, 5, 1), 9 * 60);
        const QDateTime nine(QDate(2030, 5, 1), QTime(9, 0));
        notifier.check(nine);
        QCOMPARE(alerts.sent.size(), 1);

        Reminder later = *store.reminder(id);
        later.dueMinutes = 11 * 60;
        store.put(later);
        notifier.act(1, "done", nine.addSecs(60));
        QVERIFY(!store.reminder(id)->completed);
        notifier.check(nine.addSecs(2 * 3600));
        QCOMPARE(alerts.sent.size(), 2);
        notifier.act(2, "done", nine.addSecs(2 * 3600 + 60));
        QVERIFY(store.reminder(id)->completed);

        const qint64 gone = add(store, listId, "Water the plants", QDate(2030, 5, 1), 12 * 60);
        notifier.check(nine.addSecs(3 * 3600));
        QCOMPARE(alerts.sent.size(), 3);
        store.removeReminder(gone);
        notifier.act(3, "hour", nine.addSecs(3 * 3600 + 60));
        QVERIFY(!store.reminder(gone));
        QCOMPARE(store.reminders().size(), 1);
    }

    // Undone in the window, the moment is the alert's again, and so are its
    // buttons.
    void anUndoneCompletionGivesTheAlertItsButtonsBack() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const QDateTime eight(QDate(2030, 5, 1), QTime(8, 0));
        const qint64 id = addDaily(store, "Take pills", eight);
        notifier.check(eight);
        store.complete(id, eight.addSecs(5 * 60));
        store.undo();
        QCOMPARE(store.reminder(id)->due, QDate(2030, 5, 1));

        notifier.act(1, "done", eight.addSecs(10 * 60));
        QCOMPARE(store.reminder(id)->due, QDate(2030, 5, 2));
        QCOMPARE(completedCopies(store, "Take pills"), 1);
    }

    // With the background service on, the alert is the service's while the
    // completing may be the window's: a click is weighed against what the
    // window wrote, not against what the service last heard of it.
    void aLateClickSeesWhatAnotherProcessDid() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store service(path), window(path);
        FakeAlerts alerts;
        Notifier notifier(&service, &alerts);
        const QDateTime eight(QDate(2030, 5, 1), QTime(8, 0));
        const qint64 id = addDaily(service, "Take pills", eight);
        notifier.check(eight);
        QCOMPARE(alerts.sent.size(), 1);

        QVERIFY(window.reloadIfChanged());
        window.complete(id, eight.addSecs(5 * 60));
        notifier.act(1, "done", eight.addSecs(10 * 60));
        const Store disk(path);
        QCOMPARE(disk.reminder(id)->due, QDate(2030, 5, 2));
        QCOMPARE(completedCopies(disk, "Take pills"), 1);
    }

    // An alert that has left the screen has no buttons left. Its id is the
    // desktop's to give out again: after the notification server restarts,
    // it names another app's notification.
    void aClickOnAnAlertThatLeftTheScreenIsNotOurs() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 id = add(store, addList(store, "Home"), "Call the dentist", QDate(2030, 5, 1), 9 * 60);
        const QDateTime nine(QDate(2030, 5, 1), QTime(9, 0));
        notifier.check(nine);
        notifier.forget(1);

        QSignalSpy opened(&notifier, &Notifier::opened);
        notifier.act(1, "default", nine.addSecs(60));
        notifier.act(1, "done", nine.addSecs(60));
        QCOMPARE(opened.count(), 0);
        QVERIFY(!store.reminder(id)->completed);
    }

    // Only the process that showed an alert answers its buttons. Without the
    // background service that is the window, so as it quits, each alert it
    // still has on screen is shown again in its place without them: what
    // came due stays in sight, and no button is left that nothing answers.
    void quittingTakesTheButtonsOffItsAlerts() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        const qint64 listId = addList(store, "Home");
        add(store, listId, "Call the dentist", QDate(2030, 5, 1), 9 * 60);
        add(store, listId, "Water the plants", QDate(2030, 5, 1), 9 * 60);
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(9, 0)));
        QCOMPARE(alerts.sent.size(), 2);
        notifier.forget(1); // dismissed, so not to be shown again

        notifier.disarm();
        QCOMPARE(alerts.disarmed.keys(), QList<quint32>{2});
        QCOMPARE(alerts.disarmed[2].title, alerts.sent[1].title);
        QCOMPARE(alerts.disarmed[2].body, alerts.sent[1].body);
        notifier.disarm();
        QCOMPARE(alerts.disarmed.size(), 1);
    }

    // A desktop that reads the body as markup would take a & or < in a list
    // name, a title or a note for markup of its own, so there the body is
    // escaped. One that shows the body as it is would show the escapes, so
    // there it is not. The title is never markup.
    void alertBodiesAreEscapedWhereTheyAreMarkup() {
        for (const bool markup : {true, false}) {
            QTemporaryDir dir;
            Store store(dir.filePath("nudge.db"));
            FakeAlerts alerts;
            alerts.markup = markup;
            Notifier notifier(&store, &alerts);
            Reminder r;
            r.listId = addList(store, "Tom & Jerry");
            r.title = "Cards <3";
            r.notes = "Ask for \"Love & Co\" <first>\nthen the rest";
            r.due = QDate(2030, 5, 1);
            r.dueMinutes = 9 * 60;
            store.put(r);
            notifier.check(QDateTime(QDate(2030, 5, 1), QTime(9, 0)));
            QCOMPARE(alerts.sent.size(), 1);
            QCOMPARE(alerts.sent[0].title, QString("Cards <3"));
            const QString body = alerts.sent[0].body;
            if (markup) {
                QVERIFY2(body.contains(" · Tom &amp; Jerry\n"), qPrintable(body));
                QVERIFY2(body.endsWith("\nAsk for &quot;Love &amp; Co&quot; &lt;first&gt;"), qPrintable(body));
            } else {
                QVERIFY2(body.contains(" · Tom & Jerry\n"), qPrintable(body));
                QVERIFY2(body.endsWith("\nAsk for \"Love & Co\" <first>"), qPrintable(body));
            }
        }
    }

    // The titles listed in a pile are body too.
    void aPileOfTitlesIsEscapedWhereItIsMarkup() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        FakeAlerts alerts;
        alerts.markup = true;
        Notifier notifier(&store, &alerts);
        const qint64 listId = addList(store, "Home");
        const QStringList titles{"Fish & chips", "<3 Mom", "a > b", "Plain"};
        for (const QString &title : titles)
            add(store, listId, title, QDate(2030, 5, 1), 9 * 60);
        notifier.check(QDateTime(QDate(2030, 5, 1), QTime(12, 0)));
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(alerts.sent[0].title, QString("4 reminders are due"));
        QStringList lines = alerts.sent[0].body.split('\n');
        lines.sort();
        QCOMPARE(lines, QStringList({"&lt;3 Mom", "Fish &amp; chips", "Plain", "a &gt; b"}));
    }

    // ---- The window itself ----------------------------------------------------------

    void theWindowIsDrivenFromTheKeyboard() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        backend.setClock([] { return friday; });
        Theme theme(dir.path());
        Look look(dir.path());
        Translator translator(dir.filePath("language"), ":/i18n");
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&warnings](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("look", &look);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.load(QUrl("qrc:/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto type = [window](const QString &text) {
            for (const QChar c : text)
                QTest::keyClick(window, c.toLatin1());
        };

        // N starts a reminder; Return saves it and starts the next; Escape stops.
        QTest::keyClick(window, Qt::Key_N);
        QTRY_VERIFY(window->property("editingId").toLongLong() > 0);
        type("Buy milk tomorrow #groceries");
        QTest::keyClick(window, Qt::Key_Return);
        type("Call mom");
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(window->property("editingId").toLongLong(), 0);
        QCOMPARE(store.reminders().size(), 2);
        QVERIFY(window->property("keys").toBool());

        const qint64 callMom = backend.selectedId();
        QCOMPARE(store.reminder(callMom)->title, QString("Call mom"));
        QCOMPARE(store.reminder(callMom)->due, friday.date()); // added in Today

        // Space completes, Ctrl+Z takes it back.
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(store.reminder(callMom)->completed);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!store.reminder(callMom)->completed);

        // F flags, 3 is high priority, Shift+T is tomorrow.
        QTRY_COMPARE(backend.selectedId(), callMom);
        QTest::keyClick(window, Qt::Key_F);
        QTest::keyClick(window, Qt::Key_3);
        QVERIFY(store.reminder(callMom)->flagged);
        QCOMPARE(store.reminder(callMom)->priority, 3);
        QTest::keyClick(window, Qt::Key_T, Qt::ShiftModifier);
        QCOMPARE(store.reminder(callMom)->due, QDate(2026, 11, 14));

        // Shift+N names a new list in a dialog, which has the keys to itself.
        QTest::keyClick(window, Qt::Key_N, Qt::ShiftModifier);
        QTRY_VERIFY(window->property("popupOpen").toBool());
        type("Errands");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->property("popupOpen").toBool());
        QCOMPARE(backend.viewTitle(), QString("Errands"));
        QCOMPARE(store.reminders().size(), 2); // typing the name pressed no shortcuts

        // The arrows drive the sidebar after Left, and the list again after Right.
        QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(backend.viewTitle(), QString("Reminders"));
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(window->property("pane").toString(), QString("list"));

        // G and Shift+G reach the ends of the list on keyboards without Home and End.
        backend.setViewKey("all");
        QTest::keyClick(window, Qt::Key_G, Qt::ShiftModifier);
        QCOMPARE(backend.selectedRow(), backend.reminders()->rowCount() - 1);
        QTest::keyClick(window, Qt::Key_G);
        QCOMPARE(backend.reminders()->idAt(backend.selectedRow() - 1), 0); // the first reminder, under its heading

        // Backspace deletes (a laptop's Delete key), says so, and Ctrl+Z brings it back.
        QSignalSpy notices(&backend, &Backend::notice);
        const qint64 doomed = backend.selectedId();
        QTest::keyClick(window, Qt::Key_Backspace);
        QVERIFY(!store.reminder(doomed));
        QCOMPARE(notices.count(), 1);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(store.reminder(doomed));

        // A reminder typed fast grows (the date read out of it shows beneath)
        // while the rows below are still sliding aside; they must end up below it.
        auto *list = window->findChild<QQuickItem *>("reminders");
        QVERIFY(list);
        backend.selectEdge(false);
        QTest::keyClick(window, Qt::Key_N);
        type("Water the plants tomorrow 5pm");
        const int typed = backend.selectedRow();
        auto rowAt = [list](int index) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
            return item;
        };
        QVERIFY(rowAt(typed) && rowAt(typed + 1));
        QTRY_VERIFY(rowAt(typed)->y() + rowAt(typed)->height() <= rowAt(typed + 1)->y() + 0.5);
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);

        // ? shows the shortcuts, and keys do nothing underneath it.
        QTest::keyClick(window, Qt::Key_Question, Qt::ShiftModifier);
        QVERIFY(window->property("helpVisible").toBool());
        QTest::keyClick(window, Qt::Key_N);
        QCOMPARE(store.reminders().size(), 2);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("helpVisible").toBool());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // ---- qml (review fixes) ----

private:
    // A window on a store of its own, with the clock held still and systemd
    // stood in for, so a key that lands on a shortcut reaches nothing outside.
    struct QmlWindow {
        QTemporaryDir dir;
        Store store{dir.filePath("nudge.db")};
        Backend backend{&store};
        Theme theme{dir.path()};
        Look look{dir.path()};
        Translator translator{dir.filePath("language"), ":/i18n"};
        QQmlApplicationEngine engine;
        QStringList warnings;
        QQuickWindow *window = nullptr;

        QmlWindow() {
            backend.setClock([] { return friday; });
            backend.setService(dir.filePath("systemctl"), dir.filePath("missing.service"));
        }
        bool open(bool details = false) {
            QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
                for (const QQmlError &error : errors)
                    warnings << error.toString();
            });
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("look", &look);
            engine.rootContext()->setContextProperty("backend", &backend);
            engine.load(QUrl("qrc:/Main.qml"));
            if (engine.rootObjects().size() != 1)
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window)
                return false;
            window->setProperty("detailsVisible", details);
            return QTest::qWaitForWindowExposed(window) && QTest::qWaitForWindowActive(window);
        }
        qint64 editingId() const { return window->property("editingId").toLongLong(); }
        // The first item, depth first, that matches.
        QQuickItem *find(const std::function<bool(QQuickItem *)> &match, QQuickItem *from = nullptr) const {
            QQuickItem *item = from ? from : window->contentItem();
            if (match(item))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(match, child))
                    return found;
            return nullptr;
        }
        QQuickItem *row(qint64 id) const {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(window->findChild<QQuickItem *>("reminders"), "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, backend.reminders()->rowOf(id)));
            return item;
        }
        qint64 titled(const QString &title) const {
            for (const Reminder &r : store.reminders())
                if (r.title == title)
                    return r.id;
            return 0;
        }
        void click(QQuickItem *item) const {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        }
        // Clicks a reminder's row once the rows have stopped sliding aside.
        void clickRow(qint64 id) const {
            QQuickItem *item = row(id);
            for (qreal before = -1; item->y() != before;) {
                before = item->y();
                QTest::qWait(60);
            }
            click(item);
        }
        void type(const QString &text) const {
            for (const QChar c : text)
                QTest::keyClick(window, c.toLatin1());
        }
        // Opens a dropdown and clicks one of its options, as a person would.
        bool pick(QQuickItem *dropdown, int index) const {
            click(dropdown);
            auto *popup = dropdown->property("popup").value<QObject *>();
            if (!popup || !QTest::qWaitFor([popup] { return popup->property("opened").toBool(); }))
                return false;
            auto *options = popup->property("contentItem").value<QQuickItem *>();
            QQuickItem *option = nullptr;
            const bool shown = QTest::qWaitFor([&] {
                QMetaObject::invokeMethod(options, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, option), Q_ARG(int, index));
                return option && option->width() > 0;
            });
            if (!shown)
                return false;
            click(option);
            return QTest::qWaitFor([popup] { return !popup->property("visible").toBool(); });
        }
    };

    static bool isDropdown(QQuickItem *item, int options) {
        return item->inherits("QQuickComboBox") && item->property("count").toInt() == options;
    }

private slots:
    // Text that came from elsewhere (an import, an agent, the command line)
    // shows as it is written: markup in it is not drawn, and no picture it
    // names is fetched just by being on screen.
    void markupFromElsewhereIsShownAsText() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QStringList fetched;
        connect(&server, &QTcpServer::newConnection, &server, [&server, &fetched] {
            while (QTcpSocket *socket = server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [socket, &fetched] {
                    if (!socket->canReadLine())
                        return;
                    fetched << QString::fromLatin1(socket->readLine()).section(' ', 1, 1);
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
        });
        const auto picture = [&server](const QString &name) {
            return QString("<img src=\"http://127.0.0.1:%1/%2\">").arg(server.serverPort()).arg(name);
        };

        QmlWindow w;
        const qint64 listId = addList(w.store, "Errands " + picture("list"));
        Reminder r = *w.store.reminder(add(w.store, listId, "Pick up the parcel " + picture("title")));
        r.notes = picture("notes");
        r.tags = {picture("tag")};
        r.url = picture("link");
        w.store.put(r);
        w.backend.setViewKey("list:" + QString::number(listId));
        w.backend.setSelectedId(r.id);
        QVERIFY(w.open(true));
        QVERIFY(w.row(r.id));

        // Deleting a list that holds reminders asks first, naming the list.
        QMetaObject::invokeMethod(w.window, "show", Q_ARG(QVariant, QVariant(QString("delete"))));
        QTRY_VERIFY(w.window->property("popupOpen").toBool());

        // A picture asked for on purpose: once it arrives, any other has had
        // its chance to.
        QQmlComponent component(&w.engine);
        component.setData("import QtQuick\nText { textFormat: Text.StyledText; text: '" + picture("sentinel").toUtf8() + "' }",
                          QUrl());
        std::unique_ptr<QObject> sentinel(component.create());
        QVERIFY2(sentinel, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(sentinel.get())->setParentItem(w.window->contentItem());
        QTRY_VERIFY(fetched.contains("/sentinel"));
        QTest::qWait(200);
        QCOMPARE(fetched, QStringList{"/sentinel"});
    }

    // What is typed in the details is saved to the reminder it was typed
    // for, whatever moves the selection meanwhile, and never to the next one.
    void detailsSaveTypingToItsOwnReminder() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const QDate today = friday.date();
        const qint64 a = add(w.store, home, "Alpha", today);
        const qint64 b = add(w.store, home, "Beta", today);
        const qint64 c = add(w.store, home, "Gamma", today);
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(b);
        QVERIFY(w.open(true));
        QQuickItem *notes = w.find([](QQuickItem *item) { return item->inherits("QQuickTextArea"); });
        QQuickItem *date = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("placeholderText").toString() == "Date";
        });
        QVERIFY(notes && date);

        // Clicking another reminder mid-word: Beta and Gamma both have no
        // notes, so the field reads the same for either.
        w.click(notes);
        QTRY_VERIFY(notes->hasActiveFocus());
        w.type("Bring cash");
        w.clickRow(c);
        QCOMPARE(w.backend.selectedId(), c);
        QCOMPARE(w.store.reminder(b)->notes, QString("Bring cash"));
        QCOMPARE(w.store.reminder(c)->notes, QString());
        QCOMPARE(notes->property("text").toString(), QString());

        // The selection moving on with no click, as an alert's Show or a
        // completed reminder leaving moves it.
        w.click(notes);
        QTRY_VERIFY(notes->hasActiveFocus());
        w.type("Gate");
        w.backend.setSelectedId(a);
        QCOMPARE(w.store.reminder(c)->notes, QString("Gate"));
        QCOMPARE(notes->property("text").toString(), QString());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!notes->hasActiveFocus());
        QCOMPARE(w.store.reminder(a)->notes, QString());

        // Alpha and Beta are both due "Today".
        w.click(date);
        QTRY_VERIFY(date->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("fri");
        w.backend.setSelectedId(b);
        QCOMPARE(w.store.reminder(a)->due, QDate(2026, 11, 20));
        QCOMPARE(date->property("text").toString(), QString("Today"));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!date->hasActiveFocus());
        QCOMPARE(w.store.reminder(b)->due, today);

        // A view with nothing in it leaves nothing selected for a moment.
        w.click(notes);
        QTRY_VERIFY(notes->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("Card");
        w.backend.setViewKey("flagged");
        QCOMPARE(w.store.reminder(b)->notes, QString("Card"));
        w.backend.setViewKey("list:" + QString::number(home));
        QCOMPARE(w.backend.selectedId(), a);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!notes->hasActiveFocus());
        QCOMPARE(w.store.reminder(a)->notes, QString());
        QCOMPARE(w.store.reminder(b)->notes, QString("Card"));

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // A title being typed is done when Return, Escape, or a click elsewhere
    // says so, and its keys never go to the shortcuts instead.
    void typingATitleIsNotCutShort() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        for (int i = 0; i < 60; ++i)
            add(w.store, home, QString("Item %1").arg(i));
        w.backend.setViewKey("list:" + QString::number(home));
        QVERIFY(w.open(true));
        QQuickItem *notes = w.find([](QQuickItem *item) { return item->inherits("QQuickTextArea"); });
        QVERIFY(notes);
        QSignalSpy notices(&w.backend, &Backend::notice);

        // Clicking another field keeps the title typed so far, and what is
        // typed next goes into the field clicked.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        w.type("Dentist");
        w.click(notes);
        QTRY_VERIFY(notes->hasActiveFocus());
        QCOMPARE(w.editingId(), 0);
        w.type("Bring card");
        QTest::keyClick(w.window, Qt::Key_Escape);
        const qint64 dentist = w.titled("Dentist");
        QVERIFY(dentist);
        QCOMPARE(w.store.reminder(dentist)->notes, QString("Bring card"));
        QVERIFY(w.window->property("detailsVisible").toBool());
        QCOMPARE(notices.count(), 0);

        // Another window taking the keyboard for a moment (the pointer passing
        // over it, with focus following the mouse) does not end the typing.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        w.type("Water");
        QWindow other;
        other.resize(80, 80);
        other.show();
        other.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QVERIFY(!w.window->isActive());
        QVERIFY(w.editingId() > 0);
        w.window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(w.window));
        w.type(" plants");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(w.titled("Water plants"));

        // Down while typing stays in the title.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        w.type("Tea");
        QTest::keyClick(w.window, Qt::Key_Down);
        QVERIFY(w.editingId() > 0);
        w.type("cup");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(w.titled("Teacup"));

        // Renaming a reminder scrolled out of sight brings it back to type
        // in, and never saves what an earlier rename left in its field.
        const qint64 first = w.titled("Item 0"), last = w.titled("Item 59");
        w.backend.setSelectedId(first);
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), first);
        w.type(" was here");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.store.reminder(first)->title, QString("Item 0 was here"));
        w.backend.setSelectedId(last);
        QMetaObject::invokeMethod(w.window->findChild<QQuickItem *>("reminders"), "positionViewAtBeginning");
        QTRY_VERIFY(!w.row(last));
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), last);
        QVERIFY(w.row(last));
        w.type(" later");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.editingId(), 0);
        QCOMPARE(w.store.reminder(last)->title, QString("Item 59 later"));
        QCOMPARE(w.store.reminder(first)->title, QString("Item 0 was here"));

        QCOMPARE(notices.count(), 0);
        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // The details' dropdowns change only by a choice made in their open list.
    // A pick with the mouse hands the keys back to the reminders, and keys
    // never change a closed dropdown unseen.
    void dropdownsChangeOnlyByAChoiceInTheirList() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home"), work = addList(w.store, "Work");
        const qint64 a = add(w.store, home, "Alpha", friday.date());
        const qint64 b = add(w.store, home, "Beta", friday.date());
        const qint64 c = add(w.store, home, "Gamma", friday.date());
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(a);
        QVERIFY(w.open(true));
        QQuickItem *lists = w.find([](QQuickItem *item) {
            return item->inherits("QQuickComboBox") && item->property("textRole").toString() == "name";
        });
        QQuickItem *repeat = w.find([](QQuickItem *item) { return isDropdown(item, 6); });
        QQuickItem *priority = w.find([](QQuickItem *item) { return isDropdown(item, 4); });
        QVERIFY(lists && repeat && priority);

        QVERIFY(w.pick(lists, 1));
        QCOMPARE(w.store.reminder(a)->listId, work);
        QCOMPARE(w.backend.selectedId(), b);
        QTest::keyClick(w.window, Qt::Key_Down);
        QCOMPARE(w.store.reminder(b)->listId, home);
        QCOMPARE(w.store.reminder(c)->listId, home);
        QCOMPARE(w.backend.selectedId(), c);
        QVERIFY(w.window->property("keys").toBool());

        // Clicked open and closed again with no choice, it gives the keys
        // back all the same.
        w.click(priority);
        auto *choices = priority->property("popup").value<QObject *>();
        QTRY_VERIFY(choices->property("opened").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!choices->property("visible").toBool());
        QVERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_H);
        QCOMPARE(w.store.reminder(c)->priority, 0);
        QCOMPARE(w.window->property("pane").toString(), QString("sidebar"));
        QTest::keyClick(w.window, Qt::Key_L);

        // Reached with Tab from the date, the arrows and letters open the
        // list to choose from, and only Return chooses.
        QTest::keyClick(w.window, Qt::Key_D);
        QTest::keyClick(w.window, Qt::Key_Tab);
        QTest::keyClick(w.window, Qt::Key_Tab);
        QTRY_VERIFY(repeat->hasActiveFocus());
        auto *popup = repeat->property("popup").value<QObject *>();
        w.type("d");
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(w.store.reminder(c)->repeat, QString());
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(w.store.reminder(c)->repeat, QString("daily:1"));
        QVERIFY(repeat->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_Down, Qt::AltModifier); // moving a reminder, out of habit
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(w.store.reminder(c)->repeat, QString("daily:1"));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.window->property("keys").toBool());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // Redo answers Ctrl+Shift+Z, as the shortcuts sheet says, and Ctrl+Y.
    void redoAnswersBothOfItsKeys() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const qint64 id = add(w.store, home, "Stretch");
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(id);
        QVERIFY(w.open());

        QTest::keyClick(w.window, Qt::Key_F);
        QVERIFY(w.store.reminder(id)->flagged);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(id)->flagged);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(w.store.reminder(id)->flagged);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(id)->flagged);
        QTest::keyClick(w.window, Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(w.store.reminder(id)->flagged);
    }

    // Clicking away from a new reminder finishes it before the selection
    // moves: adding and naming it stay one step to undo, and one left blank
    // goes without a trace.
    void clickingAwayFromANewReminderKeepsOneStepToUndo() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const qint64 a = add(w.store, home, "Alpha"), b = add(w.store, home, "Beta");
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(a);
        w.store.clearUndo();
        QVERIFY(w.open());

        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 milk = w.editingId();
        w.type("Buy milk");
        w.clickRow(b);
        QTRY_COMPARE(w.editingId(), 0);
        QCOMPARE(w.store.reminder(milk)->title, QString("Buy milk"));
        QCOMPARE(w.backend.selectedId(), b);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(milk));
        QCOMPARE(w.store.reminders().size(), 2);

        w.store.clearUndo();
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 blank = w.editingId();
        w.clickRow(a);
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(!w.store.reminder(blank));
        QCOMPARE(w.backend.selectedId(), a);
        QVERIFY(!w.store.canUndo());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // The repeat stepper shows every interval a repeat can have, so choosing
    // another unit keeps the interval as it was.
    void longRepeatIntervalsAreKept() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        Reminder r = *w.store.reminder(add(w.store, home, "Service the boiler", friday.date()));
        r.repeat = "daily:120";
        w.store.put(r);
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(r.id);
        QVERIFY(w.open(true));
        QQuickItem *every = w.find([](QQuickItem *item) { return item->inherits("QQuickSpinBox"); });
        QQuickItem *repeat = w.find([](QQuickItem *item) { return isDropdown(item, 6); });
        QVERIFY(every && repeat);
        QTRY_COMPARE(every->property("value").toInt(), 120);
        QVERIFY(w.pick(repeat, 3));
        QCOMPARE(w.store.reminder(r.id)->repeat, QString("weekly:120"));

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // ---- The look of the desktop -------------------------------------------------------

    void theLookFallsBackToOmarchysDefault() {
        QTemporaryDir dir;
        Look look(dir.path());
        QVERIFY(look.dark());
        QCOMPARE(look.color("background"), QColor("#1a1b26"));
        QCOMPARE(look.color("foreground"), QColor("#a9b1d6"));
        QCOMPARE(look.fontSize(), 12);
        QCOMPARE(look.fontFamily(), QString("monospace"));
        QCOMPARE(look.swatchNames().size(), 8);
        for (const QString &name : look.swatchNames())
            QVERIFY2(look.swatches().value(name).value<QColor>().isValid(), qPrintable(name));
    }

    void theLookTakesTheWholeThemePalette() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#1e66f5\"\nbackground = \"#eff1f5\"\ndark_background = \"#e3e4e8\"\n"
              "foreground = \"#4c4f69\"\nbright_foreground = '#111111' # a note\nred = \"#d20f39\"\ngreen = #40a02b\n");
        Look look(dir.path());
        QVERIFY(!look.dark());
        QCOMPARE(look.color("background"), QColor("#eff1f5"));
        QCOMPARE(look.color("panel"), QColor("#e3e4e8"));
        QCOMPARE(look.color("bright"), QColor("#111111"));
        QCOMPARE(look.color("accent"), QColor("#1e66f5"));
        QCOMPARE(look.color("accentInk"), QColor("#ffffff"));
        QCOMPARE(look.color("danger"), QColor("#d20f39"));
        QCOMPARE(look.swatches().value("green").value<QColor>(), QColor("#40a02b"));
        // Text that is secondary sits between the paper and the ink, on light themes too.
        const QColor dim = look.color("dim");
        QVERIFY(dim.lightness() < look.color("background").lightness() && dim.lightness() > look.color("foreground").lightness());
        // Controls are the foreground, thinned out, as the shell draws its own.
        QCOMPARE(look.color("controlFill").rgb(), QColor("#4c4f69").rgb());
        QCOMPARE(qRound(look.color("controlFill").alphaF() * 100), 4);
        QCOMPARE(qRound(look.color("controlBorder").alphaF() * 100), 40);
        // A theme without an orange still has one for flags.
        QVERIFY(look.swatches().value("orange").value<QColor>().isValid());
    }

    void theLookFollowsTheShellsControlsAndSize() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "background = \"#101010\"\nforeground = \"#eeeeee\"\naccent = \"#ff8800\"\n");
        write(dir.path() + "/theme/shell.toml",
              "[hyprland]\nactive-border = \"rgba(33ccffee) rgba(00ff99ee) 45deg\"\n"
              "[controls]\nnormal-fill-alpha = 0.1\nnormal-border = \"accent\"\nnormal-border-alpha = 1.0\n"
              "normal-border-width = 2\nselected-fill-alpha = 0.5\n"
              "[font]\nbase-size = 14\n"
              "[popups]\nborder = \"hyprland.active-border\"\n"
              "[menu]\nselected-text = \"#00ff00\"\nscrim-alpha = 0.25\n");
        Look look(dir.path());
        QCOMPARE(look.fontSize(), 14);
        QCOMPARE(look.controlBorderWidth(), 2);
        QCOMPARE(qRound(look.color("controlFill").alphaF() * 100), 10);
        QCOMPARE(look.color("controlBorder"), QColor("#ff8800"));          // a role
        QCOMPARE(qRound(look.color("selectedFill").alphaF() * 100), 50);
        QCOMPARE(look.color("cardBorder").rgb(), QColor("#33ccff").rgb()); // a pointer to a gradient: its first stop
        QCOMPARE(look.color("menuSelectedText"), QColor("#00ff00"));
        QCOMPARE(qRound(look.color("scrim").alphaF() * 100), 25);
    }

    void theLookFollowsAThemeSwitch() {
        // One level down, as Omarchy's own current/ is: Look watches the
        // parent too, and a busy system temp directory would keep putting
        // off its reload.
        QTemporaryDir dir;
        const QString current = dir.filePath("current");
        QDir(current).mkpath("theme");
        write(current + "/theme/colors.toml", "background = \"#101010\"\n");
        Look look(current);
        QSignalSpy changed(&look, &Look::changed);
        write(current + "/theme/colors.toml", "background = \"#fafafa\"\nforeground = \"#202020\"\n");
        QTRY_COMPARE(look.color("background"), QColor("#fafafa"));
        QVERIFY(changed.count() >= 1);
        QVERIFY(!look.dark()); // no mode given: told from the background
    }

    void theLookAsksHyprlandForBordersAndMotion() {
        QTemporaryDir dir;
        const QString command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncase \"$3\" in\n  general:border_size) echo '{\"int\": 4}' ;;\n"
                       "  animations:enabled) echo '{\"bool\": false}' ;;\nesac\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Look look(dir.path(), nullptr, command);
        QTRY_COMPARE(look.windowBorderWidth(), 4);
        QTRY_VERIFY(look.reduceMotion());
    }

    void listsAreColoredByTheThemesNames() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 first = backend.addList("Home", "");
        const qint64 second = backend.addList("Work", "");
        const qint64 chosen = backend.addList("Garden", "green");
        Look look(dir.path());
        QVERIFY(look.swatchNames().contains(store.list(first)->color));
        QVERIFY(store.list(first)->color != store.list(second)->color);
        QCOMPARE(store.list(chosen)->color, QString("green"));
        QCOMPARE(backend.viewColor(), QString("green")); // the window looks the name up in the theme
        backend.setViewKey("scheduled");
        QVERIFY(look.swatchNames().contains(backend.viewColor()));
    }

    // ---- Theme and language, from the app template -----------------------------------

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        QLocalServer hyprland;
        QVERIFY2(hyprland.listen(dir.filePath("events")), qPrintable(hyprland.errorString()));

        Theme theme(dir.path(), nullptr, command, dir.filePath("events"));
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        // Hyprland says when it has reloaded its config.
        QVERIFY(hyprland.waitForNewConnection(5000));
        hyprland.nextPendingConnection()->write("configreloaded>>\n");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json",
              R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // ---- desktop (review fixes) ----

private:
    // A counted string as the window shows it in a language, through the
    // translator the app installs and the catalogs it ships.
    static QString shown(const QByteArray &language, const char *text, int count) {
        QTemporaryDir dir;
        write(dir.filePath("language"), language + "\n");
        Translator translator(dir.filePath("language"), ":/i18n");
        QCoreApplication::installTranslator(&translator);
        const QString result = QCoreApplication::translate("Backend", text, "", count);
        QCoreApplication::removeTranslator(&translator);
        return result;
    }

    static int lineCount(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? int(file.readAll().count('\n')) : 0;
    }

private slots:
    // English standing in for a language Nudge has no catalog for counts the
    // English way, whatever that language's own rules say.
    void englishStandingInCountsTheEnglishWay() {
        // Filipino counts 2 as "one", Japanese has no "one", Russian counts 21 as "one".
        QCOMPARE(shown("fil", "Its %n reminders are deleted with it. Undo brings them back.", 2),
                 QString("Its 2 reminders are deleted with it. Undo brings them back."));
        QCOMPARE(shown("ja", "Deleted %n completed reminders", 1), QString("Deleted 1 completed reminder"));
        QCOMPARE(shown("ru", "Every %n days", 21), QString("Every 21 days"));
        // A language with a catalog keeps its own rules: French counts 0 as one.
        QCOMPARE(shown("fr", "Deleted %n completed reminders", 0), QString("0 rappel terminé supprimé"));
    }

    void aCountedStringACatalogLacksIsCountedInEnglish() {
        QTemporaryDir dir;
        write(dir.filePath("language"), "fr\n");
        write(dir.filePath("en.json"), R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.filePath("fr.json"), R"({"%n files": {"one": "%n fichier", "other": "%n fichiers"}})");
        Translator translator(dir.filePath("language"), dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 0), QString("%n clips"));
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 0), QString("%n fichier"));
    }

    // Every string the sources translate is in every catalog but English's,
    // which holds only the forms of counted strings.
    void everyCatalogTranslatesEveryString() {
        const QDir sources(SOURCE_DIRECTORY);
        static const QRegularExpression call("(?:\\b(?:qsTr|tr)\\(|QT_TRANSLATE_NOOP\\(\"\\w+\",\\s*|"
                                             "QCoreApplication::translate\\(\"\\w+\",\\s*)\"((?:[^\"\\\\]|\\\\.)*)\"");
        QStringList wanted;
        for (const QString &name : sources.entryList({"*.qml", "*.cpp"}, QDir::Files)) {
            QFile file(sources.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            auto calls = call.globalMatch(QString::fromUtf8(file.readAll()));
            while (calls.hasNext())
                wanted << calls.next().captured(1);
        }
        wanted.removeDuplicates();
        QVERIFY2(wanted.size() > 100, qPrintable(QString::number(wanted.size()) + " strings found"));

        QStringList missing;
        for (const QString &name : QDir(sources.filePath("i18n")).entryList({"*.json"}, QDir::Files)) {
            if (name == "en.json")
                continue;
            QFile file(sources.filePath("i18n/" + name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            // Compared as the sources write them, quotes and backslashes escaped.
            QStringList keys = QJsonDocument::fromJson(file.readAll()).object().keys();
            for (QString &key : keys)
                key.replace('\\', "\\\\").replace('"', "\\\"");
            for (const QString &text : wanted)
                if (!keys.contains(text))
                    missing << name + ": " + text;
        }
        QVERIFY2(missing.isEmpty(), qPrintable("Not translated:\n" + missing.join('\n')));
    }

    // Hyprland is asked again when it has reloaded its config, not every second.
    void hyprlandIsAskedWhenItReloads() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("current") && QDir(dir.path()).mkpath("hyprland"));
        const QString current = dir.filePath("current"), calls = dir.filePath("hyprland/calls"),
                      response = dir.filePath("hyprland/response"), command = dir.filePath("hyprland/hyprctl"),
                      slow = dir.filePath("hyprland/slow");
        // Reads its answer, notes the call, and with `slow` there takes a moment to give it.
        write(command, "#!/bin/sh\nanswer=$(cat '" + response.toUtf8() + "')\necho \"$3\" >> '" + calls.toUtf8() +
                           "'\n[ -e '" + slow.toUtf8() + "' ] && sleep 0.2\necho \"$answer\"\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\": 3}");
        QLocalServer hyprland;
        QVERIFY2(hyprland.listen(dir.filePath("hyprland/events")), qPrintable(hyprland.errorString()));

        Theme theme(current, nullptr, command, hyprland.fullServerName());
        QVERIFY(hyprland.waitForNewConnection(5000));
        QLocalSocket *themeEvents = hyprland.nextPendingConnection();
        Look look(current, nullptr, command, hyprland.fullServerName());
        QVERIFY(hyprland.waitForNewConnection(5000));
        QLocalSocket *lookEvents = hyprland.nextPendingConnection();
        QTRY_COMPARE(theme.radius(), 3);
        QTRY_COMPARE(look.windowBorderWidth(), 3);
        QTRY_COMPARE(lineCount(calls), 3); // the rounding, the border, and the animations
        QTest::qWait(1500);
        QCOMPARE(lineCount(calls), 3);

        // Other news, and half a line, ask nothing; the rest of the line does.
        write(response, "{\"int\": 5}");
        for (QLocalSocket *events : {themeEvents, lookEvents})
            events->write("activewindow>>kitty,~\nconfigrel");
        QTest::qWait(300);
        QCOMPARE(lineCount(calls), 3);
        for (QLocalSocket *events : {themeEvents, lookEvents})
            events->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 5);
        QTRY_COMPARE(look.windowBorderWidth(), 5);
        QTRY_COMPARE(lineCount(calls), 6);

        // A reload announced while an answer is on its way, read before the
        // change, has Hyprland asked once more.
        write(slow, "");
        for (QLocalSocket *events : {themeEvents, lookEvents})
            events->write("configreloaded>>\n");
        QTRY_COMPARE(lineCount(calls), 9);
        write(response, "{\"int\": 7}");
        for (QLocalSocket *events : {themeEvents, lookEvents})
            events->write("configreloaded>>\n");
        QTRY_COMPARE(theme.radius(), 7);
        QTRY_COMPARE(look.windowBorderWidth(), 7);
        QTRY_COMPARE(lineCount(calls), 12);
        QVERIFY(QFile::remove(slow));

        // Hyprland lets go of a listener that falls far behind; it comes
        // back, and asks, as a reload may have gone by.
        write(response, "{\"int\": 9}");
        themeEvents->disconnectFromServer();
        QTRY_VERIFY(hyprland.hasPendingConnections());
        QTRY_COMPARE(theme.radius(), 9);
        write(response, "{\"int\": 11}");
        hyprland.nextPendingConnection()->write("configreloaded>>\n");
        QTRY_COMPARE(theme.radius(), 11);
    }

    // `omarchy toggle animations` stills the window as it stills Hyprland,
    // even before Hyprland has reloaded, and without Hyprland at all.
    void theLookFollowsTheAnimationsToggle() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("current"));
        Look look(dir.filePath("current"), nullptr, {}, {}, dir.path());
        QVERIFY(!look.reduceMotion());

        // The first toggle makes the directory its flags go in.
        const QString flag = dir.filePath(".local/state/omarchy/toggles/hypr/no-animations.lua");
        QVERIFY(QDir().mkpath(QFileInfo(flag).absolutePath()));
        write(flag, "hl.config({ animations = { enabled = false } })\n");
        QTRY_VERIFY(look.reduceMotion());
        QVERIFY(QFile::remove(flag));
        QTRY_VERIFY(!look.reduceMotion());
        write(flag, "hl.config({ animations = { enabled = false } })\n");
        QTRY_VERIFY(look.reduceMotion());
    }

    // The person's own ~/.config/omarchy/shell.toml, where `omarchy display
    // text size` keeps its size, lies over the theme's and outlives a switch.
    void theLookTakesThePersonsOwnShellSettings() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("current/theme"));
        const QString themeShell = dir.filePath("current/theme/shell.toml"),
                      own = dir.filePath(".config/omarchy/shell.toml");
        write(themeShell, "[font]\nbase-size = 14\n[controls]\nnormal-border-width = 2\n");
        Look look(dir.filePath("current"), nullptr, {}, {}, dir.path());
        QCOMPARE(look.fontSize(), 14);

        QVERIFY(QDir(dir.path()).mkpath(".config/omarchy"));
        write(own, "[font]\nbase-size = 16\n");
        QTRY_COMPARE(look.fontSize(), 16);
        QCOMPARE(look.controlBorderWidth(), 2); // the theme's other settings stay

        write(themeShell, "[font]\nbase-size = 10\n");
        QTRY_COMPARE(look.controlBorderWidth(), 1);
        QCOMPARE(look.fontSize(), 16);

        // The command replaces the file, writing a new one and moving it over.
        QSaveFile replacement(own);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("[font]\nbase-size = 18\n");
        QVERIFY(replacement.commit());
        QTRY_COMPARE(look.fontSize(), 18);

        QVERIFY(QFile::remove(own));
        QTRY_COMPARE(look.fontSize(), 10);
    }

    // Made as the app makes it, the look finds the person's own settings in
    // their home. Without Hyprland's signature nothing real is asked.
    void theLookFindsThePersonsSettingsAtHome() {
        QTemporaryDir home;
        const QByteArray realHome = qgetenv("HOME"), signature = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
        const auto restore = qScopeGuard([&] {
            qputenv("HOME", realHome);
            if (!signature.isEmpty())
                qputenv("HYPRLAND_INSTANCE_SIGNATURE", signature);
        });
        qputenv("HOME", home.path().toUtf8());
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
        QVERIFY(qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && HyprlandReloads::socket().isEmpty());
        QVERIFY(QDir(home.path()).mkpath(".config/omarchy"));
        QVERIFY(QDir(home.path()).mkpath(".local/state/omarchy/toggles/hypr"));
        write(home.filePath(".config/omarchy/shell.toml"), "[font]\nbase-size = 17\n");
        write(home.filePath(".local/state/omarchy/toggles/hypr/no-animations.lua"), "");

        Look look;
        QCOMPARE(look.fontSize(), 17);
        QVERIFY(look.reduceMotion());
    }

    // Every catalog is listed in the resources, only translates text the
    // sources still use, and keeps each placeholder its English has.
    void catalogsMatchTheSources() {
        const QDir sources(SOURCE_DIRECTORY);
        QString code;
        for (const QString &name : sources.entryList({"*.qml", "*.cpp"}, QDir::Files)) {
            QFile file(sources.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            code += QString::fromUtf8(file.readAll());
        }
        QFile resources(sources.filePath("resources.qrc"));
        QVERIFY(resources.open(QIODevice::ReadOnly));
        const QString listed = QString::fromUtf8(resources.readAll());
        static const QRegularExpression placeholder("%(n|\\d)");

        const QStringList catalogs = QDir(sources.filePath("i18n")).entryList({"*.json"}, QDir::Files);
        QVERIFY(catalogs.contains("en.json"));
        for (const QString &name : catalogs) {
            QVERIFY2(listed.contains("i18n/" + name), qPrintable(name + " is not in resources.qrc"));
            QFile file(sources.filePath("i18n/" + name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError problem;
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll(), &problem).object();
            QVERIFY2(problem.error == QJsonParseError::NoError, qPrintable(name + ": " + problem.errorString()));
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = name + ": " + it.key();
                QString escaped = it.key();
                escaped.replace('\\', "\\\\").replace('"', "\\\"");
                QVERIFY2(code.contains('"' + escaped + '"'), qPrintable(where + " is not in the sources"));
                QStringList forms;
                if (it.value().isObject()) {
                    QVERIFY2(it.value().toObject().contains("other"), qPrintable(where + " has no other form"));
                    forms << it.value().toObject().value("other").toString();
                } else {
                    forms << it.value().toString();
                }
                for (const QString &form : forms) {
                    auto wanted = placeholder.globalMatch(it.key());
                    while (wanted.hasNext()) {
                        const QString mark = wanted.next().captured(0);
                        QVERIFY2(form.contains(mark), qPrintable(where + " lost " + mark));
                    }
                }
            }
        }
    }

private:
    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(NudgeTests)
#include "nudge_tests.moc"
