#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
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
#include <QThread>
#include <QTimeZone>
#include <QWheelEvent>
#include <QtTest>
#include <array>
#include <barrier>
#include <csignal>
#include <memory>
#include <sys/resource.h>
#include <sys/stat.h>
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
        if (onShow)
            onShow();
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
    std::function<void()> onShow;
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

    void quickEntryKeepsNamesAndUnsupportedSchedules() {
        const QStringList examples{"Renew The Sunday Times subscription", "Renew The Sun",
                                   "Read Midnight in the Garden of Good and Evil",
                                   "Meet every second Tuesday of the month", "Book trip from Friday to Sunday",
                                   "Cover from 5pm to 6pm"};
        for (const QString &text : examples) {
            const QuickEntry entry = Dates::parseQuick(text, friday);
            QCOMPARE(entry.title, text);
            QVERIFY(!entry.due.isValid());
            QVERIFY(entry.repeat.isEmpty());
        }
    }

    void quickEntryReadsDayPartsPunctuationAndClockWords() {
        const auto morning = Dates::parseQuick("Call tomorrow morning at 6.", friday);
        QCOMPARE(morning.title, QString("Call"));
        QCOMPARE(morning.due, friday.date().addDays(1));
        QCOMPARE(morning.minutes, 360);
        QCOMPARE(Dates::parseQuick("Call tomorrow night at 9", friday).minutes, 21 * 60);
        const auto midnight = Dates::parseQuick("Call tomorrow night at 12", friday);
        QCOMPARE(midnight.due, friday.date().addDays(2));
        QCOMPARE(midnight.minutes, 0);
        QCOMPARE(Dates::parseQuick("Call this Friday at 23:59", friday).due, friday.date());
        QCOMPARE(Dates::parseQuick("Review tomorrow.", friday).due, friday.date().addDays(1));
        const auto clock = Dates::parseQuick("Call Sam at 5 o’clock", friday);
        QCOMPARE(clock.title, QString("Call Sam"));
        QCOMPARE(clock.minutes, 17 * 60);
    }

    void localNumericDatesAndFullYearsReadAsWritten() {
        QCOMPARE(Dates::parseDate("1/2/1964", friday.date()), QDate(1964, 1, 2));
        QCOMPARE(Dates::parseDate("1/2/64", friday.date()), QDate(2064, 1, 2));
        QCOMPARE(Dates::parseQuick("Index May 4 1999", friday).due, QDate(1999, 5, 4));
        QCOMPARE(Dates::parseQuick("Pay on May 4 2101", friday).due, QDate(2101, 5, 4));
        QCOMPARE(Dates::parseQuick("Appointment on 12/10", friday).due, QDate(2026, 12, 10));
        QLocale::setDefault(QLocale("en_GB"));
        QCOMPARE(Dates::parseQuick("Appointment on 12/10", friday).due, QDate(2027, 10, 12));
        QLocale::setDefault(QLocale("de_DE"));
        QCOMPARE(Dates::parseQuick("Appointment on 24.12.", friday).due, QDate(2026, 12, 24));
        QCOMPARE(Dates::parseDate("24.12.", friday.date()), QDate(2026, 12, 24));
    }

    void dayPartsFollowWeekdaysAndWrittenDates() {
        const QDateTime monday(QDate(2031, 1, 6), QTime(9, 0));
        for (const QString day : {"Friday", "next Friday", "on 2031-01-10", "on January 10"}) {
            const auto entry = Dates::parseQuick("Call " + day + " morning at 6", monday);
            QCOMPARE(entry.title, QString("Call"));
            QCOMPARE(entry.due, day == "next Friday" ? QDate(2031, 1, 17) : QDate(2031, 1, 10));
            QCOMPARE(entry.minutes, 360);
        }
        const auto evening = Dates::parseQuick("Call Friday evening", monday);
        QCOMPARE(evening.minutes, 18 * 60);
        QCOMPARE(evening.title, QString("Call"));
        QCOMPARE(Dates::parseQuick("Read the Friday morning paper", monday).title, QString("Read the Friday morning paper"));
    }

    void midnightAtTheEndOfANightHasOneDate() {
        for (const QString time : {"12", "midnight", "12:00am", "12 am", "00:00"}) {
            const auto entry = Dates::parseQuick("Call tomorrow night at " + time, friday);
            QCOMPARE(entry.title, QString("Call"));
            QCOMPARE(entry.due, friday.date().addDays(2));
            QCOMPARE(entry.minutes, 0);
        }
        QCOMPARE(Dates::parseQuick("Call tomorrow at midnight", friday).due, friday.date().addDays(1));
    }

    void clockRangesKeepBothEndsInTheTitle() {
        for (const QString range : {"5pm to 6pm", "5 pm to 6 pm", "5 p.m. through 6pm", "5pm or 6 pm"}) {
            const QString title = "Call from " + range + " on 2031-01-15";
            const auto entry = Dates::parseQuick(title + " #work", friday);
            QCOMPARE(entry.title, title);
            QVERIFY(!entry.due.isValid());
            QCOMPARE(entry.minutes, -1);
            QCOMPARE(entry.tags, QStringList{"work"});
        }
    }

    void qualifiedWeekdayRangesStayInTheTitle() {
        for (const QString join : {"to", "through", "or"}) {
            const QString title = "Call from Friday morning " + join + " Sunday evening";
            const auto parsed = Dates::parseQuick(title + " #home", friday);
            QCOMPARE(parsed.title, title);
            QVERIFY(!parsed.due.isValid());
            QCOMPARE(parsed.minutes, -1);
            QCOMPARE(parsed.tags, QStringList{"home"});
        }
    }

    void daypartsApplyToWeeklyAndRelativeDates() {
        const auto weekly = Dates::parseQuick("Call every Friday morning at 6", friday);
        QCOMPARE(weekly.title, QString("Call"));
        QCOMPARE(weekly.minutes, 360);
        QCOMPARE(weekly.due, friday.date().addDays(7));
        QCOMPARE(weekly.repeat, QString("weekly:1"));
        const auto relative = Dates::parseQuick("Call in 2 days morning at 6", friday);
        QCOMPARE(relative.title, QString("Call"));
        QCOMPARE(relative.due, friday.date().addDays(2));
        QCOMPARE(relative.minutes, 360);
    }

    void quickEntryNeedsACueForShortNumericDates() {
        const auto fraction = Dates::parseQuick("Measure 3/4", friday);
        QCOMPARE(fraction.title, QString("Measure 3/4"));
        QVERIFY(!fraction.due.isValid());
        QCOMPARE(Dates::parseQuick("Measure on 3/4", friday).due, QDate(2027, 3, 4));
        QCOMPARE(Dates::parseQuick("Measure 2027-03-04", friday).due, QDate(2027, 3, 4));

        QLocale::setDefault(QLocale("de_DE"));
        const auto version = Dates::parseQuick("Upgrade to Python 3.12.", friday);
        QCOMPARE(version.title, QString("Upgrade to Python 3.12."));
        QVERIFY(!version.due.isValid());
        QCOMPARE(Dates::parseQuick("Upgrade Python by 3.12", friday).due, QDate(2026, 12, 3));
    }

    void quickEntryKeepsAmountsAfterNamedDates() {
        const auto pay = Dates::parseQuick("Pay contractor Dec 1 1200", friday);
        QCOMPARE(pay.title, QString("Pay contractor 1200"));
        QCOMPARE(pay.due, QDate(2026, 12, 1));
        const auto run = Dates::parseQuick("Run Nov 3 5000", friday);
        QCOMPARE(run.title, QString("Run 5000"));
        QCOMPARE(run.due, QDate(2027, 11, 3));
        QCOMPARE(Dates::parseQuick("Index Dec 1, 1200", friday).due, QDate(1200, 12, 1));
        QCOMPARE(Dates::parseDate("Dec 1 1200", friday.date()), QDate(1200, 12, 1));
    }

    void quickEntryKeepsTagSymbolsAndPreviewsEachTagOnce() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const QString text = "Study #C++ #C# #Home #home";
        const QStringList tags{"C++", "C#", "Home"};
        QCOMPARE(Dates::parseQuick(text, friday).tags, tags);
        QCOMPARE(backend.quickPreview(text), QString("#C++ · #C# · #Home"));
        const qint64 id = backend.addReminder();
        backend.commitTitle(id, text);
        QCOMPARE(store.reminder(id)->tags, tags);
    }

    void theDetailsDateCanBeReadBackInEveryLocale() {
        const QDate today(2030, 5, 3), overdue(2030, 4, 24), nextYear(2031, 12, 24);
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        backend.setClock([today] { return QDateTime(today, QTime(10, 0)); });
        const auto list = addList(store, "Home");
        const auto source = add(store, list, "Source", overdue), destination = add(store, list, "Copy");
        backend.setViewKey("list:" + QString::number(list));
        backend.setSelectedId(source);
        backend.setRepeatEnd(source, "2031-12-24");
        for (const auto *locale : {"da", "de", "es-MX", "fr", "it", "nl", "pt-BR", "sv", "en_US"}) {
            QLocale::setDefault(QLocale(locale));
            const auto fields = backend.selected();
            QVERIFY(backend.setDate(destination, fields.value("date").toString()));
            QVERIFY(backend.setRepeatEnd(destination, fields.value("repeatEnd").toString()));
            QCOMPARE(store.reminder(destination)->due, overdue);
            QCOMPARE(store.reminder(destination)->repeatEnd, nextYear);
        }
        QCOMPARE(Dates::parseDate("Wed, Apr 24", today), overdue);
        QCOMPARE(Dates::parseDate("Yesterday", today), today.addDays(-1));
    }

    void elapsedEntryKeepsTheLaterRepeatedHourOnDisk() {
        const QTimeZone zone("America/New_York");
        const auto now = QDateTime::fromString("2026-11-01T05:45:00Z", Qt::ISODate).toTimeZone(zone);
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        backend.setClock([now] { return now; });
        const auto id = backend.addReminder();
        backend.commitTitle(id, "Check in 30 minutes");
        const Store disk(store.path());
        QVERIFY(disk.reminder(id)->lateFold);
        QCOMPARE(disk.reminder(id)->dueAt(zone), now.addSecs(1800).toSecsSinceEpoch());
    }

    void movingTheMomentClearsThePreviousFold() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        Reminder entry;
        entry.listId = addList(store, "Home");
        entry.title = "After the clocks change";
        entry.due = QDate(2026, 11, 1); // Sunday
        entry.dueMinutes = 90;
        entry.lateFold = true;
        const auto id = store.put(entry);
        QVERIFY(id);
        backend.setNotes(id, "An edit that keeps the same moment");
        QVERIFY(store.reminder(id)->lateFold);

        // The store itself moves a Sunday to Monday for a weekday rule.
        backend.setRepeat(id, "weekdays", 1);
        const Store disk(store.path());
        QCOMPARE(disk.reminder(id)->due, QDate(2026, 11, 2));
        QVERIFY(!disk.reminder(id)->lateFold);
    }

    void aClockTimeOnItsOwnChoosesTheNextRealInstant() {
        const QTimeZone zone("America/New_York");
        const QDate day(2027, 11, 7);
        for (const auto &[utc, expected] : {std::pair{QTime(5, 10), QTime(5, 30)},
                                            std::pair{QTime(5, 50), QTime(6, 30)},
                                            std::pair{QTime(6, 10), QTime(6, 30)}}) {
            const auto now = QDateTime(day, utc, QTimeZone::UTC).toTimeZone(zone);
            const auto next = QDateTime(day, expected, QTimeZone::UTC);
            const auto entry = Dates::parseQuick("Check at 01:30", now);
            QCOMPARE(entry.due, day);
            QCOMPARE(entry.minutes, 90);
            QCOMPARE(entry.lateFold, expected.hour() == 6);
            QTemporaryDir dir;
            Store store(dir.filePath("reminders.db"));
            Backend backend(&store);
            quiet(backend, dir);
            backend.setClock([now] { return now; });
            const auto id = add(store, addList(store, "Home"), "Check");
            QVERIFY(backend.setTime(id, "01:30"));
            const Store disk(store.path());
            QCOMPARE(disk.reminder(id)->dueAt(zone), next.toSecsSinceEpoch());
        }
        const auto pastBoth = QDateTime(day, QTime(6, 30), QTimeZone::UTC).toTimeZone(zone);
        QCOMPARE(Dates::parseQuick("Check at 01:30", pastBoth).due, day.addDays(1));
        const QDate spring(2027, 3, 14);
        for (const auto &now : {QDateTime(spring.addDays(-1), QTime(12, 0), zone),
                                QDateTime(spring, QTime(1, 0), zone), QDateTime(spring, QTime(3, 10), zone)}) {
            const auto entry = Dates::parseQuick("Check at 02:30", now);
            QCOMPARE(entry.due, spring.addDays(1));
            QVERIFY(!entry.lateFold);
        }
    }

    void overdueUsesTheSameInstantAsTheDueAlert() {
        const QTimeZone zone("America/New_York");
        Reminder r;
        r.due = QDate(2027, 3, 14);
        r.dueMinutes = 150;
        QVERIFY(!ReminderModel::isOverdue(r, QDateTime(r.due, QTime(3, 5), zone)));
        QVERIFY(ReminderModel::isOverdue(r, QDateTime(r.due, QTime(3, 31), zone)));
        r.due = QDate(2027, 11, 7);
        r.dueMinutes = 90;
        const auto later0115 = QDateTime::fromString("2027-11-07T06:15:00Z", Qt::ISODate).toTimeZone(zone);
        QVERIFY(ReminderModel::isOverdue(r, later0115));
    }

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
        QCOMPARE(entry.due, friday.date());
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

    void aTwentyFourHourClockKeepsItsMornings() {
        // Where the clock has 24 hours, 7:30 is half past seven in the morning.
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(Dates::parseTime("7:30"), 7 * 60 + 30);
        QuickEntry entry = typedAt("Zug 6:45 tomorrow", 10);
        QCOMPARE(entry.title, QString("Zug"));
        QCOMPARE(entry.due, QDate(2026, 11, 14));
        QCOMPARE(entry.minutes, 6 * 60 + 45);
        // An hour on its own is the afternoon there too, as it always was.
        QCOMPARE(Dates::parseTime("5"), 17 * 60);
        QCOMPARE(typedAt("Call at 5", 10).minutes, 17 * 60);
        // Tonight is still the evening ahead, and its small hours the next day.
        entry = typedAt("Call mom tonight at 9:30", 19);
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 21 * 60 + 30);
        QCOMPARE(typedAt("Call mom tonight at 5:30", 10).minutes, 17 * 60 + 30);
        entry = typedAt("Check the oven tonight at 1:30", 19);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 60 + 30);

        // Where it has twelve, "at 5:30" is the afternoon, as "at 5" is.
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QCOMPARE(typedAt("meeting at 5:30", 10).minutes, 17 * 60 + 30);
        QCOMPARE(Dates::parseTime("7:30"), 19 * 60 + 30);
    }

    void aTimeReadsBackAsItIsShown() {
        // The time field shows a time the locale's way, "5:30" in Japanese and
        // Czech for half past five in the morning, and reads it back unchanged.
        for (const char *name : {"de_DE", "ja_JP", "cs_CZ", "en_US", "es_MX", "da_DK"}) {
            QLocale::setDefault(QLocale(name));
            for (int minutes = 0; minutes < 24 * 60; ++minutes) {
                const QString shown = Dates::formatTime(minutes);
                QVERIFY2(Dates::parseTime(shown) == minutes, qPrintable(QString(name) + ": " + shown));
            }
        }
    }

    void tonightWithoutATimeIsStillAhead() {
        // Once eight has gone by, tonight is the next hour on the clock...
        QuickEntry entry = typedAt("Movie tonight", 21, 30);
        QCOMPARE(entry.title, QString("Movie"));
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 22 * 60);
        entry = typedAt("Take out the trash this evening", 21, 30);
        QCOMPARE(entry.title, QString("Take out the trash"));
        QCOMPARE(entry.due, friday.date());
        QCOMPARE(entry.minutes, 22 * 60);
        // ...and after eleven, the midnight that starts the next day.
        entry = typedAt("Movie tonight", 23, 30);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 0);
        entry = typedAt("Movie this evening", 23, 30);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 0);
        // Before eight it is eight, and on another day that day's eight.
        QCOMPARE(typedAt("Movie tonight", 19, 59).minutes, 20 * 60);
        entry = typedAt("Movie tonight tomorrow", 23, 30);
        QCOMPARE(entry.due, friday.date().addDays(1));
        QCOMPARE(entry.minutes, 20 * 60);
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

    void repeatsKeepTheEndAsAnInclusiveLimit() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        const auto parsed = Dates::parseQuick("Course every 2 weeks until 2032-06-01", friday);
        QCOMPARE(parsed.due, friday.date());
        QCOMPARE(parsed.repeatEnd, QDate(2032, 6, 1));
        const CliRun ran = cliRun(path, {"add", "Course", "--due", "2032-05-04", "--repeat",
                                         "every 2 weeks until 2032-06-01", "--json"});
        QCOMPARE(ran.code, 0);
        const auto id = cliJson(ran).value("id").toInteger();
        Store store(path);
        QCOMPARE(store.reminder(id)->due, QDate(2032, 5, 4));
        QCOMPARE(store.complete(id, friday), QDate(2032, 5, 18));
        QCOMPARE(store.complete(id, friday), QDate(2032, 6, 1));
        QVERIFY(!store.complete(id, friday).isValid());
        QVERIFY(store.reminder(id)->completed);
    }

    void calendarRepeatsHandleTheWholeStoredDateRange() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto list = addList(store, "Home");
        Reminder r;
        r.listId = list;
        r.title = "Last occurrence";
        r.due = QDate(9999, 12, 31);
        r.repeat = "yearly:1:31";
        const qint64 id = store.put(r);
        QVERIFY(!store.complete(id, friday).isValid());
        const Store disk(store.path());
        QVERIFY(disk.reminder(id)->completed);
        QCOMPARE(disk.reminder(id)->due, r.due);
        QCOMPARE(Repeat::parse("daily:1").upcoming(QDate(1600, 1, 1), friday.date()), friday.date());
        QCOMPARE(Repeat::parse("monthly:1:31").upcoming(QDate(1600, 1, 31), QDate(2028, 2, 28)),
                 QDate(2028, 2, 29));
    }

    void completingARecurringParentKeepsAChildsEarlierOccurrence() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto list = addList(store, "Home");
        Reminder r;
        r.listId = list;
        r.due = friday.date();
        r.title = "Parent";
        r.repeat = "weekly:1";
        const qint64 parent = store.put(r);
        r.parentId = parent;
        r.title = "Child";
        r.repeat = "daily:1";
        const qint64 child = store.put(r);
        QVERIFY(store.complete(child, friday).isValid());
        QVERIFY(store.complete(parent, friday).isValid());
        QCOMPARE(store.childrenOf(parent), QVector<qint64>{child});
        int completed = 0;
        for (const Reminder &row : store.reminders())
            completed += row.title == "Child" && row.completed;
        QCOMPARE(completed, 1);
    }

    void closingAParentLeavesTheNextChildOccurrenceOnItsOwn() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto list = addList(store, "Home"), parent = add(store, list, "Parent");
        Reminder r;
        r.listId = list;
        r.parentId = parent;
        r.title = "Child";
        r.due = friday.date();
        r.repeat = "weekly:1";
        const auto child = store.put(r);
        store.complete(parent, friday);
        QVERIFY(store.reminder(parent)->completed);
        QCOMPARE(store.reminder(child)->parentId, 0);
        QCOMPARE(store.reminder(child)->due, friday.date().addDays(7));
        QVERIFY(!store.reminder(child)->completed);
        QCOMPARE(store.childrenOf(parent).size(), 1); // its completed child occurrence
        store.undo();
        QCOMPARE(store.reminder(child)->parentId, parent);
        QVERIFY(!store.reminder(parent)->completed);
    }

    void repeatWeekdaysAndExplicitWeekdayAnchorsSurviveDateEdits() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        const CliRun ran = cliRun(path, {"add", "Gym every Friday", "--due", "2097-05-01", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("due").toString(), QString("2097-05-03"));
        Store store(path);
        Backend backend(&store);
        quiet(backend, dir);
        const auto id = add(store, store.defaultListId(), "Task", QDate(2030, 5, 4));
        backend.setRepeat(id, "weekdays", 1);
        QCOMPARE(store.reminder(id)->due, QDate(2030, 5, 6));
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
    void restoringAListNameRetiresAnUndoOrRedoConflict_data() {
        QTest::addColumn<bool>("redo");
        QTest::newRow("undo") << false;
        QTest::newRow("redo") << true;
    }
    void restoringAListNameRetiresAnUndoOrRedoConflict() {
        QFETCH(bool, redo);
        QTemporaryDir dir;
        Store window(dir.filePath("reminders.db"));
        const auto id = addList(window, "Office");
        window.clearUndo();
        TaskList list = *window.list(id);
        list.sort = "priority";
        QVERIFY(window.put(list));
        list.name = "Work";
        QVERIFY(window.put(list));
        if (redo)
            window.undo();
        const QString held = redo ? "Work" : "Office";
        Store other(window.path());
        const auto claimed = addList(other, held);
        add(other, id, "Invoice");
        QVERIFY(window.reloadIfChanged());
        if (redo)
            window.redo();
        else
            window.undo();
        const Store disk(window.path());
        QCOMPARE(disk.list(claimed)->name, held);
        QCOMPARE(disk.list(id)->name, redo ? QString("Office") : QString("Work"));
        QVERIFY(!window.canRedo());
        QVERIFY(window.canUndo());
        window.undo();
        QCOMPARE(window.list(id)->sort, QString("manual"));
        QCOMPARE(window.list(id)->name, disk.list(id)->name);
        QVERIFY(!window.canUndo());
    }

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

    void addingAReminderKeepsUndoAndRedoForItsListsSort() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const auto list = backend.addList("Errands", "blue");
        backend.setSortMode("due");
        QCOMPARE(cliRun(store.path(), {"add", "Pick up keys", "--list", "Errands"}).code, 0);
        QVERIFY(store.reloadIfChanged());
        QVERIFY(store.canUndo());
        store.undo();
        QCOMPARE(store.list(list)->sort, QString("manual"));
        QVERIFY(!store.canUndo()); // undoing list creation would also take the new reminder
        QCOMPARE(cliRun(store.path(), {"add", "Get milk", "--list", "Errands"}).code, 0);
        QVERIFY(store.reloadIfChanged());
        QVERIFY(store.canRedo());
        store.redo();
        const Store disk(store.path());
        QCOMPARE(disk.list(list)->sort, QString("due"));
        QCOMPARE(disk.reminders().size(), 2);
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

    void movingAcrossASectionTakesTheSubtasksWithIt() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend backend(&store);
        quiet(backend, dir);
        const auto list = backend.addList("Projects", "blue");
        const auto section = backend.addSection("In progress");
        const auto parent = add(store, list, "Trip");
        Reminder child;
        child.listId = list;
        child.parentId = parent;
        child.title = "Check tickets";
        child.due = friday.date();
        child.repeat = "daily:1";
        const auto id = store.put(child);
        backend.moveReminder(parent, 1);
        QCOMPARE(Store(store.path()).reminder(id)->sectionId, section);
        store.undo();
        QCOMPARE(store.reminder(parent)->sectionId, 0);
        QCOMPARE(store.reminder(id)->sectionId, 0);
        store.redo();
        store.complete(parent, friday);
        const Store disk(store.path());
        QCOMPARE(disk.reminder(id)->parentId, 0);
        QCOMPARE(disk.reminder(id)->sectionId, section);
        QCOMPARE(disk.reminder(id)->due, friday.date().addDays(1));
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

    void oldDraftsKeepWrittenDetailsAndLoseViewDefaults() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store original(path);
        QVector<qint64> empty;
        qint64 withNotes = 0;
        {
            Backend window(&original);
            quiet(window, dir);
            for (const auto *view : {"today", "scheduled", "flagged", "tag:work"}) {
                window.setViewKey(view);
                const qint64 id = window.addReminder();
                QVERIFY(id && original.reminder(id)->draft && original.reminder(id)->hasContent());
                empty << id;
            }
            window.setViewKey("today");
            withNotes = window.addReminder();
            window.setNotes(withNotes, "What was already written");
            QVERIFY(original.reminder(withNotes)->draft);
        }
        for (const qint64 id : empty)
            age(original, id, 3600);
        age(original, withNotes, 3600);

        Store restored(path);
        Backend restarted(&restored);
        quiet(restarted, dir);
        restarted.setViewKey("all");
        QCOMPARE(restored.reminders().size(), 1);
        QCOMPARE(restored.reminder(withNotes)->notes, QString("What was already written"));
        QVERIFY(!restored.reminder(withNotes)->draft);
        QVERIFY(restarted.reminders()->rowOf(withNotes) >= 0);
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

    void cancellingADraftKeepsAConcurrentWriteAndClearsTheDraft() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend window(&store);
        quiet(window, dir);
        const qint64 list = addList(store, "Home");
        const qint64 saved = add(store, list, "Already saved");
        window.setViewKey("scheduled");
        const qint64 draft = window.addReminder();
        QVERIFY(draft && store.reminder(draft)->due.isValid());

        // This makes discardLast reload and decline to undo the old step.
        Store command(store.path());
        const qint64 outside = add(command, list, "New from another writer");
        window.commitTitle(draft, "");
        QVERIFY(!store.reminder(draft));
        QVERIFY(Store(store.path()).reminder(outside));

        // There is no separate deletion step that could bring the draft back.
        store.undo();
        QVERIFY(!store.reminder(draft));
        QVERIFY(!store.reminder(saved));
        QVERIFY(store.reminder(outside));
    }

    void aCommitThatRefusesANewReminderKeepsThePreviousSelection() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        Backend window(&store);
        quiet(window, dir);
        const qint64 list = addList(store, "Home");
        const qint64 saved = add(store, list, "Already saved");
        window.setViewKey("list:" + QString::number(list));
        QCOMPARE(window.selectedId(), saved);

        // The insert succeeds, but its deferred foreign key fails at COMMIT.
        tamper(path, "CREATE TABLE allowed (id INTEGER PRIMARY KEY)");
        tamper(path, "CREATE TABLE written (id INTEGER REFERENCES allowed(id) DEFERRABLE INITIALLY DEFERRED)");
        tamper(path, "CREATE TRIGGER record_draft AFTER INSERT ON reminders WHEN NEW.draft = 1 "
                     "BEGIN INSERT INTO written VALUES (NEW.id); END");
        for (const QString &name : QSqlDatabase::connectionNames()) {
            const QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.databaseName() == path)
                QVERIFY(QSqlQuery(db).exec("PRAGMA foreign_keys = ON"));
        }
        qint64 refused = 0;
        connect(&store, &Store::failed, &window, [&] { refused = window.draftId(); });
        QCOMPARE(window.addReminder(), 0);
        QVERIFY(refused > 0);
        QCOMPARE(window.draftId(), 0);
        QCOMPARE(window.selectedId(), saved);
        QVERIFY(!store.reminder(refused));
        QCOMPARE(Store(path).reminders().size(), 1);
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
        // A finished reminder with a subtask still open, as another process
        // can leave it until the store next opens.
        Reminder trip;
        trip.listId = home;
        trip.title = "Trip";
        const qint64 tripId = store.put(trip);
        Reminder cabin;
        cabin.listId = home;
        cabin.parentId = tripId;
        cabin.title = "Book the cabin";
        const qint64 cabinId = store.put(cabin);
        // Finished once its subtask is in: putting an open subtask under a
        // finished reminder would take that back to do.
        trip = *store.reminder(tripId);
        trip.completed = true;
        trip.completedAt = friday.toSecsSinceEpoch();
        store.put(trip);
        QVERIFY(store.reminder(tripId)->completed);
        QVERIFY(!store.reminder(cabinId)->completed);
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
        // A finished reminder with a subtask still open, as another process
        // can leave it until the store next opens.
        Reminder trip;
        trip.listId = listId;
        trip.title = "Trip";
        const qint64 tripId = store.put(trip);
        Reminder cabin;
        cabin.listId = listId;
        cabin.parentId = tripId;
        cabin.title = "Book the cabin";
        cabin.position = 1;
        const qint64 cabinId = store.put(cabin);
        // Finished once its subtask is in: putting an open subtask under a
        // finished reminder would take that back to do.
        trip = *store.reminder(tripId);
        trip.completed = true;
        trip.completedAt = friday.toSecsSinceEpoch();
        store.put(trip);
        QVERIFY(store.reminder(tripId)->completed);
        QVERIFY(!store.reminder(cabinId)->completed);
        add(store, listId, "Water the plants");

        ViewOptions options;
        options.now = friday;
        for (const View &view : {View{View::List, listId, {}}, View{View::All, 0, {}}}) {
            const QVector<Row> rows = buildRows(store, view, options);
            QVERIFY2(showsRow(rows, cabinId), qPrintable(titles(store, rows).join(", ")));
        }
        // On its own, where its parent would be, and saying whose it is.
        QCOMPARE(titles(store, buildRows(store, View{View::List, listId, {}}, options)),
                 (QStringList{"Book the cabin", "Water the plants"}));
        ReminderModel model(&store);
        model.show(buildRows(store, View{View::List, listId, {}}, options), true);
        QCOMPARE(model.data(model.index(model.rowOf(cabinId)), ReminderModel::ParentTitleRole).toString(),
                 QString("Trip"));
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
    static CliRun cliRun(const QString &database, const QStringList &arguments, const QString &language = {}) {
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
            const QStringList command = QStringList{"nudge"} + arguments;
            const QString unit = QDir(SOURCE_DIRECTORY).filePath("../pkgbuild/nudge.service");
            ran.code = language.isEmpty() ? runCli(command, unit) : runCliInDesktopLocale(command, language);
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

    void changingARepeatEndKeepsTheOriginalDay() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db");
        QCOMPARE(cliRun(path, {"add", "Month-end", "--due", "2097-01-31", "--repeat", "monthly"}).code, 0);
        QCOMPARE(cliRun(path, {"done", "1"}).code, 0);
        const auto ran = cliRun(path, {"edit", "1", "--repeat", "every month until 2097-06-30", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("repeatEnd").toString(), QString("2097-06-30"));
        QCOMPARE(cliJson(ran).value("repeat").toString(), QString("monthly:1:31"));
        QCOMPARE(cliRun(path, {"done", "1"}).code, 0);
        const Store disk(path);
        QCOMPARE(disk.reminder(1)->due, QDate(2097, 3, 31));
        const auto leap = cliRun(path, {"add", "Leap day", "--due", "2092-02-29", "--repeat", "yearly", "--json"});
        QCOMPARE(leap.code, 0);
        const auto id = QString::number(cliJson(leap).value("id").toInteger());
        QCOMPARE(cliRun(path, {"done", id}).code, 0);
        const auto sameRule = cliRun(path, {"edit", id, "--repeat", "yearly", "--json"});
        QCOMPARE(sameRule.code, 0);
        QCOMPARE(cliJson(sameRule).value("due").toString(), QString("2093-02-28"));
        QCOMPARE(cliJson(sameRule).value("repeat").toString(), QString("yearly:1:29"));
    }

    void movingTheStartOfAMonthlyRuleChangesItsAnchor() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db");
        QCOMPARE(cliRun(path, {"add", "Month-end", "--due", "2097-01-31", "--repeat", "monthly"}).code, 0);
        QCOMPARE(cliRun(path, {"done", "1"}).code, 0);
        auto changeStart = [&](const QString &date) {
            return cliRun(path, {"edit", "1", "--repeat", "every month on " + date, "--json"});
        };
        QCOMPARE(cliJson(changeStart("2097-02-28")).value("repeat").toString(), QString("monthly:1:31"));
        QCOMPARE(cliJson(changeStart("2097-03-15")).value("repeat").toString(), QString("monthly:1:15"));
        QCOMPARE(cliRun(path, {"done", "1"}).code, 0);
        QCOMPARE(Store(path).reminder(1)->due, QDate(2097, 4, 15));
    }

    void unchangedCommandOptionsKeepTheLaterFold() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db");
        const QDate day(2027, 11, 7);
        const QTimeZone zone("America/New_York");
        const auto later = QDateTime(day, QTime(1, 30), zone, QDateTime::TransitionResolution::PreferAfter).toSecsSinceEpoch();
        {
            Store store(path);
            Reminder r;
            r.listId = store.defaultListId();
            r.title = "Night shift";
            r.due = day;
            r.dueMinutes = 90;
            r.lateFold = true;
            QCOMPARE(store.put(r), 1);
            store.setAlerted(1, later, 0, 0);
        }
        for (const auto &options : {QStringList{"--time", "01:30"}, QStringList{"--due", "2027-11-07"}}) {
            QCOMPARE(cliRun(path, QStringList{"edit", "1"} + options).code, 0);
            const Store disk(path);
            QCOMPARE(disk.reminder(1)->dueAt(zone), later);
            QVERIFY(disk.reminder(1)->lateFold);
            QCOMPARE(disk.reminder(1)->alertedDue, later);
        }
        QCOMPARE(cliRun(path, {"edit", "1", "--time", "01:31"}).code, 0);
        const Store moved(path);
        QVERIFY(!moved.reminder(1)->lateFold);
        QCOMPARE(moved.reminder(1)->alertedDue, 0);
    }

    void aRepeatCannotBeEditedPastItsEnd() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db");
        QCOMPARE(cliRun(path, {"add", "Daily", "--due", "2031-01-03", "--repeat", "every day until 2031-01-04"}).code, 0);
        const auto result = cliRun(path, {"edit", "1", "--notes", "unsaved", "--due", "2031-01-05"});
        QCOMPARE(result.code, 1);
        QVERIFY(result.err.contains("after it ends"));
        Store store(path);
        Backend backend(&store);
        quiet(backend, dir);
        QSignalSpy notices(&backend, &Backend::notice);
        QVERIFY(!backend.setDate(1, "2031-01-05"));
        QTRY_VERIFY(!notices.isEmpty());
        QVERIFY(!backend.setRepeatEnd(1, "2031-01-02"));
        QCOMPARE(store.reminder(1)->repeatEnd, QDate(2031, 1, 4));
        QCOMPARE(store.reminder(1)->due, QDate(2031, 1, 3));
        QVERIFY(store.reminder(1)->notes.isEmpty());
        QVERIFY(backend.setDate(1, "2031-01-04")); // the end itself is included
        QCOMPARE(cliRun(path, {"edit", "1", "--due", "2031-01-05", "--repeat", "none"}).code, 0);
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

    void listingsSearchForHashTagsAndKeepTheirInternalSymbols() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        QCOMPARE(cliRun(path, {"add", "Homework", "--tag", "C#"}).code, 0);
        QCOMPARE(cliRun(path, {"add", "C# in a title", "--literal"}).code, 0);
        for (const QStringList &option : {QStringList{"--tag", "#C#"}, QStringList{"--search", "#c#"}}) {
            const CliRun ran = cliRun(path, QStringList{"list", "all", "--json"} + option);
            QCOMPARE(ran.code, 0);
            const auto records = cliJson(ran).value("reminders").toArray();
            QCOMPARE(records.size(), 1);
            QCOMPARE(records.at(0).toObject().value("title").toString(), QString("Homework"));
        }
    }

    void literalTitlesKeepTheirWhitespace() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), text = " Call  Sam\tat 5\ntomorrow ";
        const CliRun ran = cliRun(path, {"add", text, "--literal", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("title").toString(), text);
        QCOMPARE(Store(path).reminders().cbegin()->title, text);
    }

    void reopenReportsWhenTheActiveSeriesIsAlreadyOpen() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        const auto ran = cliRun(path, {"add", "Water", "--due", "2097-05-01", "--repeat", "daily", "--json"});
        QCOMPARE(ran.code, 0);
        const auto id = QString::number(cliJson(ran).value("id").toInteger());
        QCOMPARE(cliRun(path, {"done", id}).code, 0);
        const auto reopened = cliRun(path, {"reopen", id});
        QCOMPARE(reopened.code, 0);
        QVERIFY(reopened.out.startsWith("Already open"));
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

    void completingAParentAndChildNamesEachOccurrenceOnce() {
        for (bool childFirst : {false, true}) {
            QTemporaryDir dir;
            const auto path = dir.filePath("reminders.db");
            QCOMPARE(cliRun(path, {"add", "Parent"}).code, 0);
            QCOMPARE(cliRun(path, {"add", "Child", "--parent", "1", "--due", "2097-01-01", "--repeat", "daily"}).code, 0);
            const auto ran = cliRun(path, {"done", childFirst ? "2" : "1", childFirst ? "1" : "2", "--json"});
            QCOMPARE(ran.code, 0);
            const auto rows = cliJson(ran).value("reminders").toArray();
            QCOMPARE(rows.size(), 2);
            QCOMPARE(rows.first().toObject().value("id").toInteger(), childFirst ? 2 : 1);
            const Store disk(path);
            QCOMPARE(disk.reminders().size(), 3);
            QCOMPARE(disk.reminder(2)->due, QDate(2097, 1, 2));
            QCOMPARE(disk.reminder(2)->parentId, 0);
            QVERIFY(!disk.reminder(2)->completed);
            QCOMPARE(disk.reminder(3)->parentId, 1);
            QCOMPARE(disk.reminder(3)->due, QDate(2097, 1, 1));
            QVERIFY(disk.reminder(3)->completed);
        }
    }

    void completingAChecklistKeepsItsNextOccurrenceOpen_data() {
        QTest::addColumn<bool>("advances");
        QTest::addColumn<bool>("childFirst");
        QTest::newRow("continuing-parent-first") << true << false;
        QTest::newRow("continuing-child-first") << true << true;
        QTest::newRow("final-parent-first") << false << false;
        QTest::newRow("final-child-first") << false << true;
    }
    void completingAChecklistKeepsItsNextOccurrenceOpen() {
        QFETCH(bool, advances);
        QFETCH(bool, childFirst);
        QTemporaryDir dir;
        Store writer(dir.filePath("reminders.db"));
        Reminder parent;
        parent.listId = writer.defaultListId();
        parent.title = "Checklist";
        parent.due = QDate(2097, 1, 1);
        parent.repeat = "daily:1";
        if (!advances)
            parent.repeatEnd = parent.due;
        QCOMPARE(writer.put(parent), 1);
        Reminder child;
        child.listId = parent.listId;
        child.parentId = 1;
        child.title = "Check supplies";
        QCOMPARE(writer.put(child), 2);
        const auto result = cliRun(writer.path(), {"done", childFirst ? "2" : "1", childFirst ? "1" : "2"});
        QCOMPARE(result.code, 0);
        QVERIFY(result.out.contains("Completed 2: Check supplies"));
        const Store disk(writer.path());
        QCOMPARE(disk.reminder(1)->due, parent.due.addDays(advances ? 1 : 0));
        QCOMPARE(disk.reminder(1)->completed, !advances);
        QCOMPARE(disk.reminder(2)->completed, !advances);
    }

    void aWaitingBatchRevalidatesItsSelection_data() {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("advanced-child") << QStringList{"done", "1", "2"};
        QTest::newRow("advanced-child-first") << QStringList{"done", "2", "1"};
        QTest::newRow("deleted-row") << QStringList{"delete", "1", "2"};
    }
    void aWaitingBatchRevalidatesItsSelection() {
        QFETCH(QStringList, arguments);
        QTemporaryDir dir;
        Store writer(dir.filePath("reminders.db"));
        const bool completing = arguments.first() == "done";
        const auto list = writer.defaultListId();
        QCOMPARE(add(writer, list, "First"), 1);
        Reminder r;
        r.listId = list;
        r.parentId = completing ? 1 : 0;
        r.title = "Second";
        r.due = QDate(2097, 1, 1);
        r.repeat = "daily:1";
        QCOMPARE(writer.put(r), 2);
        Store::Tx tx(&writer, {}, Store::Latest);
        QVERIFY(tx);
        if (completing)
            QCOMPARE(writer.complete(2, friday), r.due.addDays(1));
        else
            writer.removeReminder(1);
        QProcess command;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("NUDGE_DB", writer.path());
        command.setProcessEnvironment(env);
        command.start(QDir(QCoreApplication::applicationDirPath()).filePath("../nudge"), arguments);
        QVERIFY(command.waitForStarted());
        QVERIFY(!command.waitForFinished(400));
        QVERIFY(tx.commit());
        QVERIFY(command.waitForFinished(6000));
        QCOMPARE(command.exitCode(), 1);
        QVERIFY(command.readAllStandardError().contains("Nothing was changed"));
        const Store disk(writer.path());
        QVERIFY(disk.reminder(2));
        QCOMPARE(disk.reminder(2)->due, completing ? r.due.addDays(1) : r.due);
        if (completing) {
            QCOMPARE(disk.reminders().size(), 3);
            QVERIFY(!disk.reminder(1)->completed);
            QVERIFY(!disk.reminder(2)->completed);
        }
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

    // ---- review fixes (0.1.3) ----

    void deletingAListUsesItsCurrentMembers() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store window(path);
        const qint64 home = addList(window, "Home"), work = addList(window, "Work");
        const qint64 kept = add(window, home, "Moved out");
        Store other(path);
        Reminder moved = *other.reminder(kept);
        moved.listId = work;
        QVERIFY(other.put(moved));
        const qint64 added = add(other, home, "Moved in");
        window.removeList(home);

        Store disk(path);
        QVERIFY(!disk.list(home));
        QVERIFY(disk.reminder(kept));
        QCOMPARE(disk.reminder(kept)->listId, work);
        QVERIFY(!disk.reminder(added));
        QCOMPARE(add(other, home, "Stale destination"), 0);
    }

    void mergingAnOutsideEditRetiresOlderUndoStepsForThatRow() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store window(path);
        const qint64 id = add(window, addList(window, "Home"), "Call the bank");
        Reminder flagged = *window.reminder(id);
        flagged.flagged = true;
        QVERIFY(window.put(flagged));
        Store other(path);
        Reminder noted = *other.reminder(id);
        noted.notes = "Ask about fees";
        QVERIFY(other.put(noted));
        flagged = *window.reminder(id);
        flagged.priority = 3;
        QVERIFY(window.put(flagged));
        window.reloadIfChanged();
        window.undo();
        QCOMPARE(window.reminder(id)->priority, 0);
        window.undo();
        QCOMPARE(window.reminder(id)->notes, QString("Ask about fees"));
    }

    void acceptingDeleteCompletedKeepsAParentWithANewerChild() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 listId = addList(store, "Home");
        const qint64 id = add(store, listId, "Parent");
        store.complete(id, friday);
        backend.setViewKey("list:" + QString::number(listId));
        const auto asked = backend.completedIds();
        Store other(path);
        Reminder child;
        child.title = "New subtask";
        child.parentId = id;
        child.listId = listId;
        child.completed = true;
        child.completedAt = friday.toSecsSinceEpoch();
        const qint64 childId = other.put(child);
        QVERIFY(childId);
        backend.clearCompleted(asked);
        const Store disk(path);
        QVERIFY(disk.reminder(id));
        QVERIFY(disk.reminder(childId));
        QCOMPARE(disk.reminder(childId)->parentId, id);
    }

    void aRepeatSetAfterAnOutsideDateClearStillHasADate() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        Backend backend(&store);
        quiet(backend, dir);
        backend.setClock([] { return friday; });
        const qint64 id = add(store, addList(store, "Home"), "Call", friday.date().addDays(2));
        Store other(path);
        Reminder undated = *other.reminder(id);
        undated.due = {};
        QVERIFY(other.put(undated));
        backend.setRepeat(id, "daily", 1);
        const Store disk(path);
        QCOMPARE(disk.reminder(id)->due, friday.date());
        QCOMPARE(disk.reminder(id)->repeat, QString("daily:1"));
    }

    void completingAnOccurrenceThatMovedOnDoesNotCompleteItAgain() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store window(path);
        const qint64 list = addList(window, "Home");
        const qint64 id = add(window, list, "Call", friday.date());
        Reminder repeating = *window.reminder(id);
        repeating.repeat = "daily:1";
        QVERIFY(window.put(repeating));
        Store other(path);
        QCOMPARE(other.complete(id, friday), friday.date().addDays(1));
        QVERIFY(!window.complete(id, friday).isValid());
        const Store disk(path);
        QCOMPARE(disk.reminders().size(), 2);
        QCOMPARE(disk.reminder(id)->due, friday.date().addDays(1));
    }

    void anEmptyEditorKeepsTheTitleAnotherWriterSaved() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        Backend backend(&store);
        quiet(backend, dir);
        const qint64 list = addList(store, "Home");
        backend.setViewKey("list:" + QString::number(list));
        const qint64 id = backend.addReminder();
        const CliRun ran = cliRun(path, {"edit", QString::number(id), "--title", "New title"});
        QCOMPARE(ran.code, 0);
        QVERIFY(store.reloadIfChanged());
        backend.commitTitle(id, "");
        const Store disk(path);
        QVERIFY(disk.reminder(id));
        QCOMPARE(disk.reminder(id)->title, QString("New title"));
    }

    void overrideDirectoriesKeepTheirPermissions() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("shared");
        QVERIFY(QDir().mkdir(folder));
        const auto shared = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                            QFile::ReadGroup | QFile::ExeGroup;
        QVERIFY(QFile::setPermissions(folder, shared));
        QCOMPARE(cliRun(folder + "/reminders.db", {"list"}).code, 0);
        QVERIFY(QFileInfo(folder).permissions().testFlag(QFile::ReadGroup));
        QVERIFY(!QFileInfo(folder + "/reminders.db").permissions().testFlag(QFile::ReadGroup));
    }

    void firstRemindersShareOneDefaultList() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store first(path), second(path);
        const qint64 id = first.defaultListId();
        QVERIFY(id);
        QCOMPARE(second.defaultListId(), id);
        const Store disk(path);
        QCOMPARE(disk.lists().size(), 1);
        QVERIFY(!first.canUndo());
    }

    void simultaneousFirstWritersOpenTheSameNewDatabase() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int attempt = 0; attempt < 4; ++attempt) {
            const QString path = dir.filePath(QString("reminders-%1.db").arg(attempt));
            std::barrier start(4);
            std::array<std::unique_ptr<QThread>, 4> writers;
            std::array<QString, 4> errors;
            for (size_t i = 0; i < writers.size(); ++i) {
                writers[i].reset(QThread::create([&, i] {
                    start.arrive_and_wait();
                    Store store(path);
                    Store::Tx tx(&store, {}, Store::Latest);
                    if (!tx || !add(store, store.defaultListId(), QString("First reminder %1").arg(i)) ||
                        !tx.commit())
                        errors[i] = store.error().isEmpty() ? QString("Could not save") : store.error();
                }));
                writers[i]->start();
            }
            for (const auto &writer : writers)
                writer->wait();
            for (const QString &error : errors)
                QVERIFY2(error.isEmpty(), qPrintable(error));
            const Store disk(path);
            QCOMPARE(disk.lists().size(), 1);
            QCOMPARE(disk.reminders().size(), 4);
            for (const Reminder &reminder : disk.reminders())
                QCOMPARE(reminder.listId, disk.lists().first().id);
        }
    }

    void anExactListNameWinsOverAnInsensitiveMatch() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        const qint64 home = addList(store, "Home"), upper = addList(store, "HOME");
        add(store, home, "Keep");
        add(store, upper, "Drop");
        QCOMPARE(cliRun(path, {"lists", "delete", "hOmE", "--force"}).code, 1);
        QCOMPARE(cliRun(path, {"lists", "delete", "HOME", "--force"}).code, 0);
        const Store disk(path);
        QVERIFY(disk.list(home));
        QVERIFY(!disk.list(upper));
        QCOMPARE(disk.reminders().size(), 1);
    }

    void watchingReloadsDuringContinuingDirectoryChanges() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        const qint64 id = add(store, addList(store, "Home"), "Before");
        store.watch();
        Store other(path);
        Reminder changed = *other.reminder(id);
        changed.title = "After";
        QVERIFY(other.put(changed));
        QTimer noise;
        connect(&noise, &QTimer::timeout, this, [&] {
            const QString file = dir.filePath("output");
            write(file, "output");
            QFile::remove(file);
        });
        noise.start(15);
        QTRY_COMPARE_WITH_TIMEOUT(store.reminder(id)->title, QString("After"), 500);
        QVERIFY(noise.isActive());
    }

    void aStaleWindowKeepsOthersChangesAndRestoresNothing() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store window(path), other(path);
        const qint64 listId = addList(window, "Home");
        const qint64 id = add(window, listId, "Call the bank");
        QVERIFY(other.reloadIfChanged());

        // Flagged elsewhere, and the window, not told yet, saves its notes:
        // the flag stays.
        Reminder flagged = *other.reminder(id);
        flagged.flagged = true;
        QVERIFY(other.put(flagged));
        Reminder noted = *window.reminder(id);
        noted.notes = "Ask about the fee";
        QVERIFY(window.put(noted));
        QVERIFY(window.reminder(id)->flagged);
        QVERIFY(other.reloadIfChanged());
        QVERIFY(other.reminder(id)->flagged);
        QCOMPARE(other.reminder(id)->notes, QString("Ask about the fee"));

        // Deleted elsewhere, and the window, not told yet, changes its
        // priority: it is not made again, and the window says so.
        other.removeReminder(id);
        Reminder urgent = *window.reminder(id);
        urgent.priority = 3;
        QSignalSpy failed(&window, &Store::failed);
        QCOMPARE(window.put(urgent), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!window.reminder(id));
        QVERIFY(!Store(path).reminder(id));

        // The same of a list.
        const qint64 work = addList(window, "Work");
        QVERIFY(other.reloadIfChanged());
        other.removeList(work);
        TaskList renamed = *window.list(work);
        renamed.name = "Office";
        QCOMPARE(window.put(renamed), 0);
        QVERIFY(!window.list(work));
        QVERIFY(!Store(path).list(work));
    }

    void editingNotesLeavesAnOverdueAlertDue() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        // Two reminders given a moment two hours ago, for an hour ago, which
        // has not been announced yet: the alerts were off meanwhile.
        const QDateTime hourAgo = QDateTime::currentDateTime().addSecs(-3600);
        qint64 edited = 0, untouched = 0;
        {
            Store store(path);
            const qint64 listId = addList(store, "Home");
            const int minutes = hourAgo.time().hour() * 60 + hourAgo.time().minute();
            edited = add(store, listId, "Pay the rent", hourAgo.date(), minutes);
            untouched = add(store, listId, "Water the plants", hourAgo.date(), minutes);
        }
        const qint64 twoHoursAgo = QDateTime::currentSecsSinceEpoch() - 7200;
        tamper(path, QString("UPDATE reminders SET modified_at = %1, scheduled_at = %1").arg(twoHoursAgo));

        Store store(path);
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        Reminder r = *store.reminder(edited);
        r.notes = "By transfer";
        QVERIFY(store.put(r));
        notifier.check(QDateTime::currentDateTime());
        QCOMPARE(alerts.sent.size(), 2);

        // Given a moment that has already gone by, it is not news.
        r = *store.reminder(edited);
        r.dueMinutes = r.dueMinutes - 1;
        QVERIFY(store.put(r));
        notifier.check(QDateTime::currentDateTime());
        QCOMPARE(alerts.sent.size(), 2);

        // A file from 0.1.3, with no moment noted, goes by the last edit.
        tamper(path, "UPDATE reminders SET scheduled_at = 0");
        Store older(path);
        QCOMPARE(older.reminder(untouched)->scheduledSince(), older.reminder(untouched)->modifiedAt);
    }

    void anOlderFileGainsTheMomentEachWasScheduled() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        {
            Store store(path);
            add(store, addList(store, "Home"), "Stretch");
        }
        // As Nudge 0.1.3 left it: no scheduled_at column.
        tamper(path, "ALTER TABLE reminders DROP COLUMN scheduled_at");
        tamper(path, "UPDATE reminders SET modified_at = 1234");
        Store store(path);
        QVERIFY(store.isOpen());
        QCOMPARE(store.reminders().constBegin()->scheduledAt, qint64(1234));
    }

    void simultaneousOpenersShareAnOlderFilesMigration() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        {
            Store store(path);
            add(store, addList(store, "Home"), "Stretch");
        }
        tamper(path, "ALTER TABLE reminders DROP COLUMN scheduled_at");
        tamper(path, "UPDATE reminders SET modified_at = 1234");
        {
            auto blocker = QSqlDatabase::addDatabase("QSQLITE", "migration-blocker");
            blocker.setDatabaseName(path);
            QVERIFY(blocker.open());
            QSqlQuery gate(blocker);
            QVERIFY(gate.exec("BEGIN IMMEDIATE"));
            std::array<qint64, 4> scheduled{};
            {
                std::array<std::unique_ptr<QThread>, 4> readers;
                for (size_t i = 0; i < readers.size(); ++i) {
                    readers[i].reset(QThread::create([&, i] {
                        Store store(path);
                        if (store.isOpen() && !store.reminders().isEmpty())
                            scheduled[i] = store.reminders().cbegin()->scheduledAt;
                    }));
                    readers[i]->start();
                }
                QThread::msleep(100);
                gate.exec("COMMIT");
                for (const auto &reader : readers)
                    reader->wait();
            }
            for (qint64 moment : scheduled)
                QCOMPARE(moment, qint64(1234));
        }
        QSqlDatabase::removeDatabase("migration-blocker");
    }

    void remindersAreReadableByTheirOwnerAlone() {
        const mode_t umaskBefore = ::umask(022);
        const auto restore = qScopeGuard([umaskBefore] { ::umask(umaskBefore); });
        const auto others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther |
                            QFile::WriteOther | QFile::ExeOther;
        QTemporaryDir dir;
        const QString folder = dir.filePath("nudge"), path = folder + "/nudge.db";
        {
            Store store(path);
            add(store, addList(store, "Home"), "See the doctor");
            QCOMPARE(QFileInfo(folder).permissions() & others, QFile::Permissions());
            for (const QString &file : {path, path + "-wal", path + "-shm"})
                if (QFileInfo::exists(file))
                    QVERIFY2(!(QFileInfo(file).permissions() & others), qPrintable(file));
        }
        // A file an earlier Nudge left readable by everyone is made private.
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup |
                                                QFile::ReadOther));
        {
            Store store(path);
            QCOMPARE(QFileInfo(path).permissions() & others, QFile::Permissions());
        }
        // And so is a backup, new or written over one that was not.
        const QString backup = dir.filePath("backup.json");
        QCOMPARE(cliRun(path, {"export", backup}).code, 0);
        QCOMPARE(QFileInfo(backup).permissions() & others, QFile::Permissions());
        QVERIFY(QFile::setPermissions(backup, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        QCOMPARE(cliRun(path, {"export", backup}).code, 0);
        QCOMPARE(QFileInfo(backup).permissions() & others, QFile::Permissions());
    }

    void aBackupKeepsListsAndSectionsApart() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        {
            Store store(path);
            const qint64 home = addList(store, "Home"), shouting = addList(store, "HOME");
            qint64 later[2] = {};
            for (int i = 0; i < 2; ++i) {
                Section section;
                section.listId = home;
                section.name = "Later";
                section.position = i + 1;
                later[i] = store.put(section);
            }
            for (int i = 0; i < 2; ++i) {
                Reminder r;
                r.listId = home;
                r.sectionId = later[i];
                r.title = i ? "Second" : "First";
                r.position = store.endPosition(home);
                QVERIFY(store.put(r));
            }
            add(store, shouting, "Loud");
        }
        const QString file = dir.filePath("backup.json");
        QCOMPARE(cliRun(path, {"export", file}).code, 0);

        // Restored into an empty file, two lists and two sections stay two.
        auto restoredFrom = [&dir](const QString &backup, const QString &name) {
            const QString into = dir.filePath(name);
            const CliRun ran = cliRun(into, {"import", backup});
            if (ran.code != 0)
                qWarning() << ran.err;
            return into;
        };
        {
            Store restored(restoredFrom(file, "restored.db"));
            QStringList names;
            for (const TaskList &list : restored.lists())
                names << list.name;
            QCOMPARE(names, (QStringList{"Home", "HOME"}));
            QCOMPARE(restored.sections().size(), 2);
            QHash<QString, const Reminder *> byTitle;
            for (const Reminder &r : restored.reminders())
                byTitle.insert(r.title, &r);
            QCOMPARE(byTitle.size(), 3);
            QVERIFY(byTitle["First"]->sectionId && byTitle["Second"]->sectionId);
            QVERIFY(byTitle["First"]->sectionId != byTitle["Second"]->sectionId);
            QCOMPARE(byTitle["First"]->listId, byTitle["Second"]->listId);
            QVERIFY(byTitle["Loud"]->listId != byTitle["First"]->listId);
        }

        // An export from before ids keeps lists apart that differ in case.
        QFile original(file);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QJsonObject document = QJsonDocument::fromJson(original.readAll()).object();
        QJsonArray lists, reminders;
        for (const QJsonValue &value : document.value("lists").toArray()) {
            QJsonObject entry = value.toObject();
            entry.remove("id");
            entry.remove("sectionIds");
            lists << entry;
        }
        for (const QJsonValue &value : document.value("reminders").toArray()) {
            QJsonObject entry = value.toObject();
            entry.remove("listId");
            entry.remove("sectionId");
            reminders << entry;
        }
        document["lists"] = lists;
        document["reminders"] = reminders;
        const QString older = dir.filePath("older.json");
        write(older, QJsonDocument(document).toJson());
        Store restored(restoredFrom(older, "older.db"));
        QCOMPARE(restored.lists().size(), 2);

        // Imported over lists that are here already, it uses each only once.
        const QString existing = dir.filePath("existing.db");
        {
            Store store(existing);
            addList(store, "home");
        }
        restoredFrom(file, "existing.db");
        Store again(existing);
        QCOMPARE(again.lists().size(), 2);
    }

    void deleteCompletedDeletesWhatItAskedAbout() {
        QTemporaryDir dir;
        Store store(dir.filePath("nudge.db"));
        Backend backend(&store);
        const qint64 listId = addList(store, "Home");
        const qint64 asked = add(store, listId, "Asked about");
        store.complete(asked, friday);
        backend.setViewKey("list:" + QString::number(listId));
        const QVariantList ids = backend.completedIds();
        QCOMPARE(ids, (QVariantList{asked}));
        // Completed while the question is up, here or by a command.
        const qint64 meanwhile = add(store, listId, "Done meanwhile");
        store.complete(meanwhile, friday);
        QCOMPARE(backend.completedCount(), 2);
        backend.clearCompleted(ids);
        QVERIFY(!store.reminder(asked));
        QVERIFY(store.reminder(meanwhile));
    }

    void commandsReadTimesAsTheWindowDoes() {
        QTemporaryDir dir;
        const QLocale before;
        const auto restore = qScopeGuard([before] { QLocale::setDefault(before); });
        const QString language = dir.filePath("language");
        for (const QByteArray &code : {QByteArray("en"), QByteArray("de")}) {
            write(language, code + "\n");
            int inWindow = -1;
            {
                Translator translator(language, ":/i18n");
                inWindow = Dates::parseTime("5:30");
            }
            // Whatever the system's own locale is.
            QLocale::setDefault(QLocale(code == "en" ? "de" : "en"));
            Translator::useDesktopLocale(language);
            QCOMPARE(Dates::parseTime("5:30"), inWindow);
        }
    }

    void listingAViewOfOneListShowsThatViewOfIt() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        {
            Store store(path);
            const qint64 home = addList(store, "Home"), work = addList(store, "Work");
            const QDate today = QDate::currentDate();
            add(store, home, "Home today", today);
            add(store, home, "Home tomorrow", today.addDays(1));
            add(store, work, "Work today", today);
        }
        auto listed = [&path](const QStringList &arguments) {
            QStringList found;
            for (const QJsonValue &r : cliJson(cliRun(path, arguments)).value("reminders").toArray())
                found << r.toObject().value("title").toString();
            found.sort();
            return found;
        };
        QCOMPARE(listed({"list", "today", "--list", "Home", "--json"}), QStringList{"Home today"});
        QCOMPARE(listed({"list", "--list", "Home", "--json"}), (QStringList{"Home today", "Home tomorrow"}));
        QCOMPARE(listed({"list", "today", "--json"}), (QStringList{"Home today", "Work today"}));
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

    void exportRefusesTheDatabaseAndItsAliases() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        const qint64 id = add(store, addList(store, "Home"), "Keep this");
        const QString alias = dir.filePath("alias.json"), hardLink = dir.filePath("hard.json");
        QVERIFY(QFile::link(path, alias));
        QCOMPARE(::link(QFile::encodeName(path).constData(), QFile::encodeName(hardLink).constData()), 0);
        for (const auto &destination : {path, alias, hardLink, path + "-wal", path + "-shm"})
            QCOMPARE(cliRun(path, {"export", destination, "--json"}).code, 1);
        const Store disk(path);
        QVERIFY(disk.isOpen());
        QCOMPARE(disk.reminder(id)->title, QString("Keep this"));
        QVERIFY(QFileInfo(alias).isSymLink());
    }

    void backupsKeepTheInstantsOfHistoryAndSnoozes() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), copy = dir.filePath("restored.db");
        const auto instant = QDateTime::fromString("2026-02-01T23:30:00-07:00", Qt::ISODate);
        const auto snooze = instant.addDays(1);
        {
            Store store(path);
            const auto list = addList(store, "Home");
            Reminder first;
            first.listId = list;
            first.title = "Call";
            first.due = instant.date();
            first.createdAt = instant.toSecsSinceEpoch();
            const auto id = store.put(first);
            store.setAlerted(id, 0, 0, snooze.toSecsSinceEpoch());
            store.complete(add(store, list, "Done"), instant);
        }
        const QString backup = dir.filePath("backup.json");
        QCOMPARE(cliRun(path, {"export", backup}).code, 0);
        QFile file(backup);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray json = file.readAll();
        QVERIFY(json.contains("2026-02-02T06:30:00Z"));
        QVERIFY(json.contains("2026-02-03T06:30:00Z"));
        QCOMPARE(cliRun(copy, {"import", backup}).code, 0);
        Store restored(copy);
        for (const Reminder &r : restored.reminders()) {
            if (r.title == "Call") {
                QCOMPARE(r.createdAt, instant.toSecsSinceEpoch());
                QCOMPARE(r.snoozeUntil, snooze.toSecsSinceEpoch());
            } else {
                QCOMPARE(r.completedAt, instant.toSecsSinceEpoch());
            }
        }
        QCOMPARE(restored.reminders().size(), 2);
    }

    void importReservesExactListNamesBeforeFoldingCase() {
        for (const bool withIds : {false, true}) {
            QTemporaryDir dir;
            const QString path = dir.filePath("reminders.db"), backup = dir.filePath("input.json");
            Store existing(path);
            const qint64 upper = addList(existing, "HOME");
            QJsonObject home{{"name", "Home"}}, upperHome{{"name", "HOME"}};
            QJsonObject first{{"title", "Small"}, {"list", "Home"}}, second{{"title", "Big"}, {"list", "HOME"}};
            if (withIds) {
                home["id"] = 1;
                first["listId"] = 1;
                upperHome["id"] = 2;
                second["listId"] = 2;
            }
            write(backup, QJsonDocument(QJsonObject{{"lists", QJsonArray{home, upperHome}},
                                                  {"reminders", QJsonArray{first, second}}}).toJson());
            QCOMPARE(cliRun(path, {"import", backup}).code, 0);
            Store restored(path);
            QCOMPARE(restored.lists().size(), 2);
            for (const Reminder &r : restored.reminders())
                QCOMPARE(r.listId == upper, r.title == "Big");
        }
    }

    void importRejectsBrokenBackupsBeforeChangingLists() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), file = dir.filePath("input.json");
        Store store(path);
        add(store, addList(store, "Home"), "Keep this");
        const QVector<QJsonValue> malformed{
            QJsonObject{{"title", "An object"}},
            QJsonArray{QJsonObject{{"id", 7}, {"title", "Repeated"}},
                       QJsonObject{{"id", 7}, {"title", "Another"}}},
            QJsonArray{QJsonObject{{"parent", 9}, {"title", "Orphan"}}},
            QJsonArray{"A reminder should be an object"},
        };
        for (const QJsonValue &reminders : malformed) {
            write(file, QJsonDocument(QJsonObject{{"reminders", reminders},
                {"lists", QJsonArray{QJsonObject{{"name", "Never created"}}}}}).toJson());
            QCOMPARE(cliRun(path, {"import", file}).code, 1);
        }
        const Store disk(path);
        QCOMPARE(disk.lists().size(), 1);
        QCOMPARE(disk.reminders().size(), 1);
    }

    void oldBackupsKeepUntitledRemindersWithContent() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), file = dir.filePath("old.json");
        write(file, R"({
          "nudge": 1,
          "lists": [{"name": "Home", "color": "blue", "sections": []}],
          "reminders": [
            {"id": 1, "title": "", "notes": "Words worth keeping", "list": "Home"},
            {"id": 2, "title": "  ", "due": "2030-05-01", "url": "example.test", "tags": ["work"], "list": "Home"},
            {"id": 3, "title": "", "list": "Home"},
            {"id": 4, "title": "Subtask", "parent": 3, "list": "Home"},
            {"id": 5, "title": " ", "notes": "  ", "tags": [" "], "list": "Home"}
          ]
        })");
        const CliRun result = cliRun(path, {"import", file, "--json"});
        QCOMPARE(result.code, 0);
        QCOMPARE(cliJson(result).value("imported").toInt(), 4);
        QCOMPARE(cliJson(result).value("skipped").toInt(), 1);

        Store store(path);
        Backend window(&store);
        quiet(window, dir);
        QCOMPARE(store.reminders().size(), 4);
        QCOMPARE(store.reminder(1)->title, QString("Untitled reminder"));
        QCOMPARE(store.reminder(1)->notes, QString("Words worth keeping"));
        QCOMPARE(store.reminder(2)->due, QDate(2030, 5, 1));
        QCOMPARE(store.reminder(2)->url, QString("example.test"));
        QCOMPARE(store.reminder(2)->tags, QStringList{"work"});
        QCOMPARE(store.reminder(4)->parentId, 3);
        QCOMPARE(store.reminder(3)->title, QString("Untitled reminder"));
    }

    void importRejectsInvalidDatesAndTimesWithoutChangingTheDestination() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db"), file = dir.filePath("input.json");
        QCOMPARE(cliRun(path, {"add", "Keep this"}).code, 0);
        const auto before = cliJson(cliRun(path, {"export"}));
        const QJsonObject row{{"title", "Check filter"}, {"due", "2031-02-28"}, {"time", "09:00"}};
        for (const auto &[field, value] : QVector<QPair<QString, QJsonValue>>{
                 {"due", "2031-02-30"}, {"due", ""}, {"due", 2031}, {"time", "09:70"}, {"time", false},
                 {"repeatEnd", "2031-02-29"}, {"createdAt", "unknown"}, {"completedAt", "2031-02-30T09:00Z"},
                 {"snoozeUntil", "tomorrow"}}) {
            QJsonObject invalid = row;
            invalid.insert(field, value);
            write(file, QJsonDocument(QJsonObject{{"reminders", QJsonArray{row, invalid}},
                                                  {"lists", QJsonArray{QJsonObject{{"name", "Incoming"}}}}}).toJson());
            QCOMPARE(cliRun(path, {"import", file}).code, 1);
            const auto after = cliJson(cliRun(path, {"export"}));
            QCOMPARE(after.value("lists"), before.value("lists"));
            QCOMPARE(after.value("reminders"), before.value("reminders"));
        }
        write(file, R"({"reminders":[{"title":"No date","due":null,"time":null},{"title":"Date omitted"}]})");
        QCOMPARE(cliRun(path, {"import", file}).code, 0);
    }

    void importRejectsDuplicateListIdsBeforeMergingLists() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db"), file = dir.filePath("input.json");
        QCOMPARE(cliRun(path, {"lists", "add", "Home", "--color", "blue"}).code, 0);
        QCOMPARE(cliRun(path, {"add", "Keep this", "--list", "Home"}).code, 0);
        const auto before = cliJson(cliRun(path, {"export"}));
        write(file, R"({"lists":[{"id":1,"name":"Home","color":"red"},{"id":1,"name":"Travel","color":"green"}],
                        "reminders":[{"title":"Passport","listId":1,"list":"Travel"}]})");
        const auto ran = cliRun(path, {"import", file});
        QCOMPARE(ran.code, 1);
        QVERIFY(ran.err.contains("duplicate list ids"));
        const auto after = cliJson(cliRun(path, {"export"}));
        QCOMPARE(after.value("lists"), before.value("lists"));
        QCOMPARE(after.value("reminders"), before.value("reminders"));
    }

    void aDatelessRepeatInAnOldBackupStartsToday() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), file = dir.filePath("old.json");
        write(file, R"({"reminders": [
            {"title": "A weekly reminder", "due": null, "repeat": "weekly:1"},
            {"title": "A weekday reminder", "repeat": "weekdays"},
            {"title": "Another reminder"}
        ]})");
        const QDate today = QDate::currentDate();
        const CliRun result = cliRun(path, {"import", file, "--json"});
        QCOMPARE(result.code, 0);
        QCOMPARE(cliJson(result).value("imported").toInt(), 3);
        const Store store(path);
        QCOMPARE(store.reminder(1)->due, today);
        QCOMPARE(store.reminder(1)->repeat, QString("weekly:1"));
        QCOMPARE(store.reminder(2)->due, Repeat::firstWorkday(today));
        QCOMPARE(store.reminder(2)->repeat, QString("weekdays"));
        QVERIFY(!store.reminder(3)->due.isValid());
    }

    void importPreservesSavedRepeatSchedules_data() {
        QTest::addColumn<QByteArray>("schedule");
        QTest::addColumn<QDate>("due");
        QTest::newRow("weekend") << QByteArray(R"("due":"2031-02-01","repeat":"weekdays","repeatEnd":"2031-02-02")")
                                 << QDate(2031, 2, 1);
        QTest::newRow("past-end") << QByteArray(R"("due":"2031-02-03","repeat":"daily:1","repeatEnd":"2031-02-02")")
                                  << QDate(2031, 2, 3);
        QTest::newRow("ended-dateless") << QByteArray(R"("due":null,"repeat":"weekly:1","repeatEnd":"2001-02-02")")
                                        << QDate();
    }
    void importPreservesSavedRepeatSchedules() {
        QFETCH(QByteArray, schedule);
        QFETCH(QDate, due);
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db"), file = dir.filePath("old.json");
        write(file, R"({"reminders":[{"title":"Another reminder"},{"title":"Saved repeat",)" + schedule + "}]}");
        QCOMPARE(cliRun(path, {"import", file}).code, 0);
        const Store disk(path);
        QCOMPARE(disk.reminders().size(), 2);
        QCOMPARE(disk.reminder(2)->due, due);
        const auto original = QJsonDocument::fromJson("{" + schedule + "}").object();
        QCOMPARE(disk.reminder(2)->repeatEnd.toString(Qt::ISODate), original.value("repeatEnd").toString());
        QCOMPARE(disk.reminder(2)->repeat, original.value("repeat").toString());
        QCOMPARE(cliRun(path, {"edit", "2", "--notes", "Keep schedule"}).code, 0);
        QCOMPARE(cliRun(path, {"edit", "2", "--due", "2031-02-05"}).code, 1);
    }

    void importChecksSectionIdsAndTheirLists_data() {
        QTest::addColumn<QByteArray>("ids");
        QTest::addColumn<QByteArray>("reference");
        QTest::newRow("duplicate") << QByteArray("[7,7]") << QByteArray();
        QTest::newRow("not-an-id") << QByteArray("[7,0]") << QByteArray();
        QTest::newRow("wrong-list") << QByteArray("[7,8]") << QByteArray(R"(,"listId":2,"list":"Other","sectionId":8,"section":"Filed")");
        QTest::newRow("wrong-name") << QByteArray("[7,8]") << QByteArray(R"(,"listId":1,"list":"Home","sectionId":7,"section":"Filed")");
        QTest::newRow("unknown") << QByteArray("[7,8]") << QByteArray(R"(,"listId":1,"list":"Home","sectionId":42,"section":"Filed")");
    }
    void importChecksSectionIdsAndTheirLists() {
        QFETCH(QByteArray, ids);
        QFETCH(QByteArray, reference);
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db"), file = dir.filePath("sections.json");
        QCOMPARE(cliRun(path, {"add", "Keep this"}).code, 0);
        const auto before = cliJson(cliRun(path, {"export"}));
        write(file, R"({"lists":[{"id":1,"name":"Home","sections":["Plan","Filed"],"sectionIds":)" + ids +
                    R"(},{"id":2,"name":"Other"}],"reminders":[{"title":"Invoice")" + reference + "}]}");
        QCOMPARE(cliRun(path, {"import", file}).code, 1);
        QCOMPARE(cliJson(cliRun(path, {"export"})), before);
        write(file, R"({"lists":[{"name":"Home","sections":["Filed"]}],"reminders":[{"title":"Invoice","list":"Home","section":"Filed"}]})");
        QCOMPARE(cliRun(path, {"import", file}).code, 0);
        const Store restored(path);
        QCOMPARE(restored.section(restored.reminder(2)->sectionId)->name, QString("Filed"));
    }

    void aSnoozeInABackupNeedsAnInstant() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db"), file = dir.filePath("backup.json");
        QCOMPARE(cliRun(path, {"add", "Keep this"}).code, 0);
        const auto before = cliJson(cliRun(path, {"export"}));
        for (const QByteArray stamp : {"2031-04-01", "2031-04-01T08:15:00"}) {
            write(file, R"({"reminders":[{"title":"Call","due":"2031-04-02","snoozeUntil":")" + stamp + R"("}]})");
            QCOMPARE(cliRun(path, {"import", file}).code, 1);
            QCOMPARE(cliJson(cliRun(path, {"export"})), before);
        }
        write(file, R"({"reminders":[{"title":"Call","due":"2031-04-02","snoozeUntil":"2031-04-01T08:15:00-06:00",
                     "createdAt":"2030-04-02T09:10:00"},{"title":"No snooze","snoozeUntil":null}]})");
        QCOMPARE(cliRun(path, {"import", file}).code, 0);
        const Store disk(path);
        QCOMPARE(disk.reminder(2)->snoozeUntil, QDateTime(QDate(2031, 4, 1), QTime(14, 15), QTimeZone::UTC).toSecsSinceEpoch());
        QCOMPARE(disk.reminder(2)->createdAt, QDateTime(QDate(2030, 4, 2), QTime(9, 10)).toSecsSinceEpoch());
    }

    void deletingAListMatchesItsSpacing() {
        QTemporaryDir dir;
        const auto path = dir.filePath("reminders.db");
        Store writer(path);
        const auto one = addList(writer, "Work Notes"), two = addList(writer, "Work  Notes");
        const auto keep = add(writer, one, "Keep invoice"), remove = add(writer, two, "Archive receipt");
        QCOMPARE(cliRun(path, {"lists", "delete", "Work  Notes", "--force"}).code, 0);
        const Store disk(path);
        QVERIFY(disk.list(one));
        QVERIFY(disk.reminder(keep));
        QVERIFY(!disk.list(two));
        QVERIFY(!disk.reminder(remove));
        writer.reloadIfChanged();
        addList(writer, "Work Notes");
        QCOMPARE(cliRun(path, {"lists", "delete", "Work Notes", "--force"}).code, 1);
        QVERIFY(Store(path).reminder(keep));
    }

    void showingAndExportingAnUntitledReminderUseTheSameTitle() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Reminder entry;
        entry.listId = addList(store, "Home");
        entry.title = "  ";
        entry.notes = "Written notes";
        const auto id = store.put(entry);
        const auto shown = cliJson(cliRun(store.path(), {"show", QString::number(id), "--json"}));
        const auto exported = cliJson(cliRun(store.path(), {"export"})).value("reminders").toArray();
        QCOMPARE(exported.size(), 1);
        QCOMPARE(shown.value("title").toString(), QString("Untitled reminder"));
        QCOMPARE(exported.first().toObject().value("title"), shown.value("title"));
    }

    void importAppendsRowsAndOnlyMapsRealParentIds() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), file = dir.filePath("input.json");
        Store store(path);
        const auto list = addList(store, "Home");
        const auto previous = add(store, list, "Earlier");
        write(file, R"({"reminders": [
            {"title": "First", "parent": null, "list": "Home"},
            {"title": "Parent", "id": 7, "list": "Home"},
            {"title": "Child", "parent": 7, "list": "Home"},
            {"title": "Last", "list": "Home"}]})");
        QCOMPARE(cliRun(path, {"import", file}).code, 0);
        Store restored(path);
        QCOMPARE(restored.reminders().size(), 5);
        QCOMPARE(restored.childrenOf(previous).size(), 0);
        ViewOptions options;
        options.now = friday;
        QCOMPARE(titles(restored, buildRows(restored, View::fromKey("list:" + QString::number(list)), options)),
                 (QStringList{"Earlier", "First", "Parent", "  Child", "Last"}));
    }

    void installingAKitChecksBothDestinationsBeforeOverwriting() {
        QTemporaryDir dir;
        const QByteArray original = qgetenv("HOME");
        qputenv("HOME", QFile::encodeName(dir.path()));
        const auto restore = qScopeGuard([&] { qputenv("HOME", original); });
        const QString primary = dir.filePath(".agents/skills/nudge/SKILL.md");
        const QString alternate = dir.filePath(".claude/skills/nudge");
        QVERIFY(QDir().mkpath(QFileInfo(primary).path()));
        QVERIFY(QDir().mkpath(QFileInfo(alternate).path()));
        write(primary, "Keep this");
        write(alternate, "Occupied");
        QCOMPARE(cliRun(dir.filePath("reminders.db"), {"skill", "install", "--json"}).code, 1);
        QFile previous(primary);
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), QByteArray("Keep this"));
        previous.close();
        QVERIFY(QFile::remove(alternate));
        QVERIFY(QFile::setPermissions(QFileInfo(alternate).path(), QFile::ReadOwner | QFile::ExeOwner));
        QCOMPARE(cliRun(dir.filePath("reminders.db"), {"skill", "install", "--json"}).code, 1);
        QVERIFY(QFile::setPermissions(QFileInfo(alternate).path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), QByteArray("Keep this"));
    }

    void installingAKitResolvesRelocatedAndSharedSkillDirectories() {
        for (const auto &layout : {QString("nested/elsewhere/skills"), QString(".agents/skills")}) {
            QTemporaryDir dir;
            const QByteArray original = qgetenv("HOME");
            qputenv("HOME", QFile::encodeName(dir.path()));
            const auto restore = qScopeGuard([&] { qputenv("HOME", original); });
            QVERIFY(QDir().mkpath(dir.filePath(".claude")));
            QVERIFY(QDir().mkpath(dir.filePath(layout)));
            QVERIFY(QFile::link(dir.filePath(layout), dir.filePath(".claude/skills")));
            const CliRun ran = cliRun(dir.filePath("reminders.db"), {"skill", "install", "--json"});
            QCOMPARE(ran.code, 0);
            const QString active = cliJson(ran).value("linked").toString() + "/SKILL.md";
            const QString installed = cliJson(ran).value("installed").toString();
            QVERIFY(QFile::exists(active));
            QCOMPARE(QFileInfo(active).canonicalFilePath(), QFileInfo(installed).canonicalFilePath());
        }
    }

    void typingForAPictureChangesOnlyItsCopy() {
        QTemporaryDir dir;
        const QString source = dir.filePath("reminders.db"), copy = dir.filePath("picture.db");
        Store real(source);
        const qint64 list = addList(real, "Home");
        add(real, list, "From the original");
        QCOMPARE(Store::copyForPicture(source, copy), QString());
        Store picture(copy);
        QVERIFY(picture.isOpen());
        QCOMPARE(picture.reminders().size(), 1);
        Backend backend(&picture);
        quiet(backend, dir);
        const auto id = backend.addReminder();
        backend.commitTitle(id, "Words for the picture tomorrow");
        QCOMPARE(picture.reminders().size(), 2);
        QCOMPARE(Store(source).reminders().size(), 1);
    }

    void aScreenshotSelectsTheRequestedReminder() {
        QTemporaryDir dir;
        const auto db = dir.filePath("reminders.db");
        Store store(db);
        const auto list = addList(store, "Home");
        const auto first = add(store, list, "First example"), second = add(store, list, "Second example");
        QProcessEnvironment env;
        env.insert("HOME", dir.path());
        env.insert("PATH", dir.path());
        env.insert("TMPDIR", dir.path());
        env.insert("XDG_RUNTIME_DIR", dir.path());
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("QT_QUICK_BACKEND", "software");
        env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + dir.filePath("no-session"));
        env.insert("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + dir.filePath("no-system"));
        env.insert("NUDGE_DB", db);
        const auto nudge = QDir(QCoreApplication::applicationDirPath()).filePath("../nudge");
        auto picture = [&](qint64 id, QImage &image) {
            const auto path = dir.filePath(QString::number(id) + ".png");
            QStringList args{"open", "--view", "all", "--details", "--now", "2031-01-15T09:00", "--screenshot", path};
            if (id)
                args << "--reminder" << QString::number(id);
            QProcess process;
            process.setProcessEnvironment(env);
            process.start(nudge, args);
            QVERIFY2(process.waitForFinished() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                     process.readAllStandardError().constData());
            QVERIFY(image.load(path));
        };
        QImage initial, firstSelected, secondSelected;
        picture(0, initial);
        picture(first, firstSelected);
        picture(second, secondSelected);
        QCOMPARE(firstSelected, initial);
        QVERIFY(secondSelected != firstSelected);
    }

    void aFailedPicturePreservesItsDestination_data() {
        QTest::addColumn<QString>("filename");
        QTest::addColumn<qint64>("id");
        QTest::newRow("missing-reminder") << QString("reminder.png") << qint64(999);
        QTest::newRow("unsupported-format") << QString("backup.json") << qint64(1);
        QTest::newRow("partial-write") << QString("reminder.bmp") << qint64(1);
    }
    void aFailedPicturePreservesItsDestination() {
        QFETCH(QString, filename);
        QFETCH(qint64, id);
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        add(store, store.defaultListId(), "Appointment");
        const auto output = dir.filePath(filename);
        const QByteArray original = "Keep the existing file\n";
        write(output, original);
        QProcess command;
        QProcessEnvironment env;
        for (const QString key : {"HOME", "PATH", "TMPDIR", "XDG_RUNTIME_DIR"})
            env.insert(key, dir.path());
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("QT_QUICK_BACKEND", "software");
        env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + dir.filePath("no-bus"));
        env.insert("DBUS_SYSTEM_BUS_ADDRESS", env.value("DBUS_SESSION_BUS_ADDRESS"));
        env.insert("NUDGE_DB", store.path());
        command.setProcessEnvironment(env);
        if (filename.endsWith(".bmp")) {
            // Enough room for the database copy, less than one uncompressed window.
            command.setChildProcessModifier([] {
                const rlimit limit{256 * 1024, 256 * 1024};
                if (::setrlimit(RLIMIT_FSIZE, &limit) != 0)
                    ::_exit(125);
                std::signal(SIGXFSZ, SIG_IGN);
            });
        }
        command.start(QDir(QCoreApplication::applicationDirPath()).filePath("../nudge"),
                      {"open", "--view", "all", "--details", "--size", "800x600", "--reminder", QString::number(id),
                       "--screenshot", output});
        QVERIFY(command.waitForFinished());
        QCOMPARE(command.exitStatus(), QProcess::NormalExit);
        QCOMPARE(command.exitCode(), 1);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        const auto error = command.readAllStandardError();
        QVERIFY(error.contains(id == 999 ? QByteArray("999") : filename.toUtf8()));
    }

    void anOlderUntitledReminderWithDetailsSurvivesStartup() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto id = add(store, addList(store, "Home"), "", friday.date());
        age(store, id, 3600);
        Backend backend(&store);
        quiet(backend, dir);
        QVERIFY(store.reminder(id));
        QCOMPARE(store.reminder(id)->due, friday.date());
    }

    void anUnfinishedDraftDoesNotAppearInAnotherWindowOrBackup() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db");
        Store store(path);
        Backend first(&store);
        quiet(first, dir);
        const auto id = first.addReminder(); // Today gave it a date
        QVERIFY(store.reminder(id)->due.isValid());
        QCOMPARE(first.reminders()->rowOf(id), 0);
        Store read(path);
        Backend second(&read);
        quiet(second, dir);
        QCOMPARE(second.openCount(), 0);
        QCOMPARE(second.reminders()->rowOf(id), -1);
        const auto exported = cliJson(cliRun(path, {"export"}));
        QVERIFY(exported.value("reminders").toArray().isEmpty());
    }

    void firstCliUseNamesTheDefaultListInTheDesktopLanguage() {
        QTemporaryDir dir;
        const QString language = dir.filePath("language"), path = dir.filePath("reminders.db");
        write(language, "de\n");
        const CliRun ran = cliRun(path, {"add", "Notiz", "--json"}, language);
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("list").toString(), QString("Erinnerungen"));
        QCOMPARE(Store(path).lists().first().name, QString("Erinnerungen"));
    }

    void aSaveConflictExplainsWhatHappenedInTheDesktopLanguage() {
        QTemporaryDir dir;
        const QString path = dir.filePath("reminders.db"), language = dir.filePath("language");
        write(language, "de\n");
        Translator translator(language);
        QCoreApplication::installTranslator(&translator);
        const auto uninstall = qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        Store window(path);
        Backend backend(&window);
        quiet(backend, dir);
        const auto id = add(window, addList(window, "Home"), "Note");
        Store other(path);
        other.removeReminder(id);
        QSignalSpy messages(&backend, &Backend::notice);
        backend.setNotes(id, "Change");
        QTRY_COMPARE(messages.count(), 1);
        const QString message = messages.first().first().toString();
        QVERIFY(message.contains("anderen Fenster"));
        QVERIFY(!message.contains("It was deleted"));
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

    void backgroundStatusDistinguishesDisabledFromAnError() {
        QTemporaryDir dir;
        const auto stub = dir.filePath("systemctl"), db = dir.filePath("reminders.db");
        const auto previousPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
        qputenv("PATH", QFile::encodeName(dir.path()));
        auto systemctl = [&](const QByteArray &response) {
            write(stub, "#!/bin/sh\n" + response + '\n');
            QVERIFY(QFile::setPermissions(stub, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        };
        for (const QByteArray state : {"disabled", "masked", "masked-runtime"}) {
            systemctl("echo " + state + "\nexit 1");
            const auto ran = cliRun(db, {"background", "--json"});
            QCOMPARE(ran.code, 0);
            QCOMPARE(cliJson(ran).value("background"), QJsonValue(false));
        }
        systemctl("echo enabled\necho warning >&2");
        const auto enabled = cliRun(db, {"background", "--json"});
        QCOMPARE(enabled.code, 0);
        QCOMPARE(cliJson(enabled).value("background"), QJsonValue(true));
        systemctl("echo 'Failed to connect to user bus' >&2\nexit 1");
        const auto failed = cliRun(db, {"background", "--json"});
        QCOMPARE(failed.code, 1);
        QVERIFY(failed.out.isEmpty());
        QVERIFY(failed.err.contains("Failed to connect to user bus"));
    }

    // Worked out only: nothing is started.
    void theWindowAndCommandAgreeOnBackgroundEnablement() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        Backend window(&store);
        const auto stub = dir.filePath("systemctl"), unit = dir.filePath("nudge.service");
        write(unit, "[Service]\n");
        const auto previous = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", previous); });
        qputenv("PATH", QFile::encodeName(dir.path()));
        for (const QByteArray state : {"enabled-runtime", "disabled", "unknown"}) {
            write(stub, "#!/bin/sh\necho " + state + (state == "enabled-runtime" ? "\nexit 0\n" : "\nexit 1\n"));
            QVERIFY(QFile::setPermissions(stub, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            window.setService(stub, unit);
            const auto command = cliRun(store.path(), {"background", "--json"});
            QCOMPARE(window.backgroundOn(), state == "enabled-runtime");
            QCOMPARE(window.backgroundAsk(), state == "disabled");
            QCOMPARE(command.code, state == "unknown" ? 1 : 0);
            if (command.code == 0)
                QCOMPARE(cliJson(command).value("background").toBool(), window.backgroundOn());
        }
    }

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

    void openingAnAlertRevealsItsReminderHereOrPassesItsIdToANewWindow() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db")), other(dir.filePath("other.db"));
        const auto list = addList(store, "Home");
        const auto id = add(store, list, "Show this");
        Backend backend(&store);
        quiet(backend, dir);
        QVERIFY(!backend.revealReminder(other.path(), id));
        QVERIFY(backend.revealReminder(store.path(), id));
        QCOMPARE(backend.selectedId(), id);
        QCOMPARE(backend.viewListId(), list);
        const auto direct = windowLaunch("/usr/bin/nudge", {}, {}, id);
        QCOMPARE(direct.arguments, (QStringList{"open", "--reminder", QString::number(id)}));
        const auto scoped = windowLaunch("/usr/bin/nudge", {}, "/usr/bin/systemd-run", id);
        QCOMPARE(scoped.arguments.last(), QString::number(id));
        QVERIFY(scoped.arguments.contains("--reminder"));
    }

    void openingAnAlertForADeletedReminderStillRaisesItsWindow() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto list = addList(store, "Home");
        const auto gone = add(store, list, "Was due");
        const auto kept = add(store, list, "Still here");
        Backend window(&store);
        quiet(window, dir);
        window.setViewKey("list:" + QString::number(list));
        window.setSelectedId(kept);

        Store command(store.path());
        command.removeReminder(gone);
        QVERIFY(store.reminder(gone)); // the watcher has yet to read the change
        QSignalSpy raised(&window, &Backend::raise);
        QVERIFY(window.revealReminder(store.path(), gone));
        QCOMPARE(raised.count(), 1);
        QCOMPARE(window.selectedId(), kept);
    }

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

    // Started as `nohup nudge open &` from a script, the window keeps
    // ignoring the hangup and interrupt it was started ignoring. Only how
    // each signal is handled is looked at, and the test's own handling comes
    // back afterwards, whatever happens here.
    void aSignalIgnoredFromTheStartStaysIgnored() {
        const int stops[] = {SIGTERM, SIGINT, SIGHUP};
        struct sigaction saved[3];
        for (int i = 0; i < 3; ++i)
            QCOMPARE(sigaction(stops[i], nullptr, &saved[i]), 0);
        const auto restore = qScopeGuard([&] {
            for (int i = 0; i < 3; ++i)
                sigaction(stops[i], &saved[i], nullptr);
        });
        std::signal(SIGTERM, SIG_DFL);
        std::signal(SIGINT, SIG_IGN);
        std::signal(SIGHUP, SIG_IGN);
        quitOnStop(*QCoreApplication::instance());

        struct sigaction now;
        for (const int ignored : {SIGINT, SIGHUP}) {
            QCOMPARE(sigaction(ignored, nullptr, &now), 0);
            QVERIFY2(now.sa_handler == SIG_IGN, qPrintable(QString("signal %1").arg(ignored)));
        }
        // What it was not told to ignore still quits through the event loop.
        QCOMPARE(sigaction(SIGTERM, nullptr, &now), 0);
        QVERIFY(now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN);
    }

    void aTagWrittenTwiceIsOneTag() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        // As quick entry in the window has it: one tag, spelled as first written.
        CliRun ran = cliRun(path, {"add", "Call #Home about #home stuff", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("tags").toVariant().toStringList(), QStringList{"Home"});
        QCOMPARE(Store(path).reminder(cliJson(ran).value("id").toInteger())->tags, QStringList{"Home"});

        // And a tag is the word the window takes, commas and all.
        ran = cliRun(path, {"add", "Errands #home,office", "--json"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(cliJson(ran).value("tags").toVariant().toStringList(), QStringList{"home,office"});
    }

    void deletingAParentAndItsSubtaskNamesBoth() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nudge.db");
        Store store(path);
        const qint64 listId = addList(store, "Home");
        auto withSubtask = [&](const QString &title, const QString &subtask) {
            Reminder r;
            r.listId = listId;
            r.parentId = add(store, listId, title);
            r.title = subtask;
            r.position = store.endPosition(listId);
            return QStringList{QString::number(r.parentId), QString::number(store.put(r))};
        };
        const QStringList trip = withSubtask("Trip", "Pack"), move = withSubtask("Move", "Hire a van");

        // The subtask goes with its parent, before its own turn comes, and is
        // still reported by name.
        CliRun ran = cliRun(path, {"delete", trip[0], trip[1]});
        QCOMPARE(ran.code, 0);
        QCOMPARE(ran.out, QString("Deleted %1: Trip\nDeleted %2: Pack\n").arg(trip[0], trip[1]));

        ran = cliRun(path, {"delete", move[0], move[1], "--json"});
        QCOMPARE(ran.code, 0);
        const QJsonArray gone = cliJson(ran).value("reminders").toArray();
        QCOMPARE(gone.size(), 2);
        QCOMPARE(gone.at(0).toObject().value("title").toString(), QString("Move"));
        QCOMPARE(gone.at(1).toObject().value("title").toString(), QString("Hire a van"));
        QVERIFY(gone.at(1).toObject().value("deleted").toBool());
        QVERIFY(Store(path).reminders().isEmpty());
    }

    // ---- Alerts -------------------------------------------------------------------

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

    void anOlderEarlyMarkIsCheckedAgainstItsScheduledTime() {
        QTemporaryDir dir;
        const auto path = dir.filePath("upgrade.db");
        const auto due = QDateTime(QDate(2031, 1, 15), QTime(10, 0), QTimeZone::UTC);
        {
            Store store(path);
            const auto id = add(store, store.defaultListId(), "Appointment", due.date(), 600);
            auto r = *store.reminder(id);
            r.early = 30;
            QVERIFY(store.put(r));
        }
        // The 0.1.3 schema and the row left by Undo after delivery at a 60-minute lead.
        for (const QString column : {"scheduled_at", "late_fold", "early_scheduled_at", "draft"})
            tamper(path, "ALTER TABLE reminders DROP COLUMN " + column);
        tamper(path, QString("UPDATE reminders SET alerted_early = %1, modified_at = %2")
                         .arg(due.addSecs(-3600).toSecsSinceEpoch()).arg(due.addDays(-1).toSecsSinceEpoch()));
        FakeAlerts alerts;
        {
            Store upgraded(path);
            QVERIFY(upgraded.isOpen());
            Notifier notifier(&upgraded, &alerts);
            notifier.check(due.addSecs(-1801));
            QCOMPARE(alerts.sent.size(), 0);
            notifier.check(due.addSecs(-1800));
            QCOMPARE(alerts.sent.size(), 1);
            QCOMPARE(upgraded.reminder(1)->alertedEarly, due.addSecs(-1800).toSecsSinceEpoch());
        }
        Store reopened(path);
        Notifier notifier(&reopened, &alerts);
        notifier.check(QDateTime(due.date(), QTime(9, 30), QTimeZone("America/Chicago")));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void anExpiredSnoozeCanBeSnoozedAfterAWriteLock_data() {
        QTest::addColumn<bool>("retry");
        QTest::newRow("snooze-again") << false;
        QTest::newRow("retry-before-snoozing") << true;
    }
    void anExpiredSnoozeCanBeSnoozedAfterAWriteLock() {
        QFETCH(bool, retry);
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto due = QDateTime(QDate(2097, 1, 15), QTime(10, 0));
        const auto id = add(store, store.defaultListId(), "Appointment", due.date(), 600);
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(due);
        notifier.act(1, "hour", due);
        const auto wake = due.addSecs(3600);
        QCOMPARE(store.reminder(id)->snoozeUntil, wake.toSecsSinceEpoch());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "snooze-lock");
            db.setDatabaseName(store.path());
            QVERIFY(db.open());
            QSqlQuery lock(db);
            QVERIFY(lock.exec("BEGIN IMMEDIATE"));
            notifier.check(wake);
            QCOMPARE(alerts.sent.size(), 2);
            QVERIFY(lock.exec("ROLLBACK"));
        }
        QSqlDatabase::removeDatabase("snooze-lock");
        if (retry) {
            notifier.check(wake);
            QCOMPARE(alerts.sent.size(), 2);
            QCOMPARE(Store(store.path()).reminder(id)->snoozeUntil, 0);
        }
        notifier.act(2, "hour", wake);
        QCOMPARE(Store(store.path()).reminder(id)->snoozeUntil, wake.addSecs(3600).toSecsSinceEpoch());
        notifier.check(wake.addSecs(3599));
        QCOMPARE(alerts.sent.size(), 2);
        notifier.check(wake.addSecs(3600));
        QCOMPARE(alerts.sent.size(), 3);
    }

    void eachQueuedAlertChecksTheReminderAgain() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto due = QDateTime(QDate(2097, 1, 15), QTime(10, 0));
        const auto list = store.defaultListId();
        add(store, list, "First", due.date(), 600);
        const auto second = add(store, list, "Second", due.date(), 600);
        const auto third = add(store, list, "Third", due.date(), 600);
        Store writer(store.path());
        FakeAlerts alerts;
        alerts.onShow = [&] {
            if (alerts.sent.size() == 1) {
                writer.complete(second, due);
                retitle(writer, third, "Updated");
            }
        };
        Notifier notifier(&store, &alerts);
        notifier.check(due);
        QCOMPARE(alerts.sent.size(), 2);
        QCOMPARE(alerts.sent.last().title, QString("Updated"));
        const Store disk(store.path());
        QVERIFY(disk.reminder(second)->completed);
        QCOMPARE(disk.reminder(second)->alertedDue, 0);
        QCOMPARE(disk.reminder(third)->alertedDue, due.toSecsSinceEpoch());
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

    void snoozingAnEarlyAlertCoversTheDueMoment() {
        for (const QTimeZone &zone : {QTimeZone(QTimeZone::UTC), QTimeZone(QTimeZone::LocalTime)}) {
            for (const auto *action : {"hour", "tomorrow"}) {
                QTemporaryDir dir;
                Store store(dir.filePath("reminders.db"));
                const auto due = QDateTime(QDate::currentDate().addDays(2), QTime(10, 0), zone);
                const auto id = add(store, addList(store, "Home"), "Call", due.date(), 10 * 60);
                Reminder r = *store.reminder(id);
                r.early = 30;
                QVERIFY(store.put(r));
                FakeAlerts alerts;
                Notifier notifier(&store, &alerts);
                const auto early = due.addSecs(-1800);
                notifier.check(early.addSecs(-1));
                QCOMPARE(alerts.sent.size(), 0);
                notifier.check(early);
                QCOMPARE(alerts.sent.size(), 1);
                notifier.act(1, action, early);
                notifier.check(due);
                QCOMPARE(alerts.sent.size(), 1);
                const auto returned = QDateTime::fromSecsSinceEpoch(store.reminder(id)->snoozeUntil, zone);
                notifier.check(returned.addSecs(-1));
                QCOMPARE(alerts.sent.size(), 1);
                notifier.check(returned);
                notifier.check(returned.addSecs(60));
                QCOMPARE(alerts.sent.size(), 2);
                QCOMPARE(store.reminder(id)->alertedDue, due.toSecsSinceEpoch());
            }
        }
    }

    void anEarlyPileNamesUpcomingRemindersInOrder() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto day = QDate::currentDate().addDays(2);
        const auto list = addList(store, "Home");
        QStringList titles;
        for (int i = 1; i <= 4; ++i) {
            const auto id = add(store, list, QString("Call %1").arg(i), day, 10 * 60);
            Reminder r = *store.reminder(id);
            r.early = 30;
            QVERIFY(store.put(r));
            titles << r.title;
        }
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(QDateTime(day, QTime(9, 30)));
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(alerts.sent.first().title, QString("4 reminders are coming up"));
        QCOMPARE(alerts.sent.first().body, titles.join('\n'));
        notifier.check(QDateTime(day, QTime(10, 0)));
        QCOMPARE(alerts.sent.last().title, QString("4 reminders are due"));
    }

    void changingAnEarlyLeadLeavesAnOutstandingDueAlert() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto now = QDateTime::currentDateTime();
        const auto missed = now.addSecs(-3600);
        const auto id = add(store, addList(store, "Home"), "Overdue", missed.date(),
                            missed.time().hour() * 60 + missed.time().minute());
        const auto earlier = now.addSecs(-7200).toSecsSinceEpoch();
        tamper(store.path(), QString("UPDATE reminders SET scheduled_at = %1, early_scheduled_at = %1").arg(earlier));
        store.reloadIfChanged();
        Reminder r = *store.reminder(id);
        r.early = 5;
        QVERIFY(store.put(r));
        QCOMPARE(store.reminder(id)->scheduledAt, earlier);
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(now);
        QCOMPARE(alerts.sent.size(), 1);
    }

    void aClockRollbackCannotAnnounceAnOccurrenceAlreadyDue() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const auto due = QDateTime(QDate::currentDate().addDays(2), QTime(10, 0));
        const auto id = add(store, addList(store, "Home"), "Once", due.date(), 10 * 60);
        Reminder r = *store.reminder(id);
        r.early = 30;
        QVERIFY(store.put(r));
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(due);
        notifier.check(due.addSecs(-900));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void travellingWestKeepsDeliveryWithTheOccurrence() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        const QDate day(2097, 1, 16);
        const QTimeZone east("America/New_York"), west("America/Chicago");
        const QDateTime easternDue(day, QTime(9, 0), east), westernDue(day, QTime(9, 0), west);
        Reminder r;
        r.listId = addList(store, "Home");
        r.title = "Daily";
        r.due = day;
        r.repeat = "daily:1";
        r.early = 1440;
        const auto id = store.put(r);
        FakeAlerts alerts;
        Notifier notifier(&store, &alerts);
        notifier.check(easternDue.addDays(-1));
        QCOMPARE(alerts.sent.size(), 1);
        notifier.check(westernDue.addDays(-1));
        QCOMPARE(alerts.sent.size(), 1);
        notifier.check(easternDue);
        QCOMPARE(alerts.sent.size(), 2);
        Store reopened(store.path());
        Notifier afterTravel(&reopened, &alerts);
        afterTravel.check(westernDue);
        QCOMPARE(alerts.sent.size(), 2);
        notifier.act(2, "done", westernDue);
        QCOMPARE(store.reminder(id)->due, day.addDays(1));
        notifier.check(westernDue.addDays(1));
        QCOMPARE(alerts.sent.last().title, QString("Daily"));
        QCOMPARE(store.reminder(id)->alertedDue, westernDue.addDays(1).toSecsSinceEpoch());
    }

    void anEarlyLeadEditedDuringDeliveryKeepsTheRightMarks() {
        for (const bool atDue : {true, false}) {
            QTemporaryDir dir;
            Store store(dir.filePath("reminders.db"));
            const QDateTime due(QDate(2097, 1, 16), QTime(9, 0));
            Reminder r;
            r.listId = addList(store, "Work");
            r.title = "Send report";
            r.due = due.date();
            r.dueMinutes = 9 * 60;
            r.early = 60;
            const auto id = store.put(r);
            Store editor(store.path());
            FakeAlerts alerts;
            alerts.onShow = [&] {
                if (alerts.sent.size() == 1) {
                    Reminder changed = *editor.reminder(id);
                    changed.early = 30;
                    QCOMPARE(editor.put(changed), id);
                }
            };
            Notifier notifier(&store, &alerts);
            notifier.check(atDue ? due : due.addSecs(-3600));
            QCOMPARE(alerts.sent.size(), 1);
            const Store disk(store.path());
            QCOMPARE(disk.reminder(id)->early, 30);
            QCOMPARE(disk.reminder(id)->alertedDue, atDue ? due.toSecsSinceEpoch() : 0);
            QCOMPARE(disk.reminder(id)->alertedEarly, 0);
            notifier.check(atDue ? due.addSecs(60) : due.addSecs(-1800));
            QCOMPARE(alerts.sent.size(), atDue ? 1 : 2);
        }
    }

    void aSnoozeCheckSeesACompletionBeforeTheWatcherDoes() {
        QTemporaryDir dir;
        Store window(dir.filePath("reminders.db"));
        const auto due = QDateTime(QDate::currentDate().addDays(2), QTime(10, 0));
        const auto id = add(window, addList(window, "Home"), "Call", due.date(), 10 * 60);
        FakeAlerts alerts;
        Notifier notifier(&window, &alerts);
        notifier.check(due);
        notifier.act(1, "hour", due);
        QCOMPARE(alerts.sent.size(), 1);
        QCOMPARE(cliRun(window.path(), {"done", QString::number(id)}).code, 0);
        QVERIFY(!window.reminder(id)->completed);
        notifier.check(due.addSecs(3600));
        QCOMPARE(alerts.sent.size(), 1);
    }

    void allDayLeadsStayAtNineAcrossClockChanges() {
        const QTimeZone zone("America/New_York");
        for (const QDate &day : {QDate(2030, 3, 10), QDate(2030, 11, 3)}) {
            for (const int days : {1, 7}) {
                QTemporaryDir dir;
                Store store(dir.filePath("reminders.db"));
                const auto id = add(store, addList(store, "Home"), "All day", day);
                Reminder r = *store.reminder(id);
                r.early = days * 1440;
                QVERIFY(store.put(r));
                const auto early = QDateTime(day.addDays(-days), QTime(9, 0), zone);
                const auto earlier = early.addDays(-2).toSecsSinceEpoch();
                tamper(store.path(), QString("UPDATE reminders SET scheduled_at = %1, early_scheduled_at = %1").arg(earlier));
                FakeAlerts alerts;
                Notifier notifier(&store, &alerts);
                notifier.check(early.addSecs(-60));
                QCOMPARE(alerts.sent.size(), 0);
                notifier.check(early);
                QCOMPARE(alerts.sent.size(), 1);
                QCOMPARE(store.reminder(id)->alertedEarly, early.toSecsSinceEpoch());
            }
        }
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
    void wheelScrollingKeepsTheInlineEditorWithItsReminder() {
        QmlWindow w;
        const auto list = addList(w.store, "Home");
        qint64 last = 0;
        for (int i = 0; i < 90; ++i)
            last = add(w.store, list, QString("Reminder %1").arg(i));
        w.backend.setViewKey("list:" + QString::number(list));
        QVERIFY(w.open());
        QTest::keyClick(w.window, Qt::Key_End);
        QTRY_COMPARE(w.backend.selectedId(), last);
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), last);
        auto *view = w.window->findChild<QQuickItem *>("reminders");
        QVERIFY(view);
        QTRY_VERIFY(!view->property("interactive").toBool());
        const QPointF position = view->mapToScene(QPointF(20, 20));
        for (int i = 0; i < 6; ++i) {
            QWheelEvent wheel(position, position, {}, QPoint(0, 480), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(w.window, &wheel);
        }
        w.type(" XYZ");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), 0);
        QCOMPARE(w.store.reminder(last)->title, QString("Reminder 89 XYZ"));
    }

    void retranslationKeepsDetailsChoices() {
        QmlWindow w;
        const auto list = addList(w.store, "Home"), id = add(w.store, list, "Repeat", friday.date());
        Reminder r = *w.store.reminder(id);
        r.repeat = "weekly:1";
        r.repeatEnd = friday.date().addDays(20);
        r.priority = 3;
        QVERIFY(w.store.put(r));
        w.backend.setViewKey("list:" + QString::number(list));
        w.backend.setSelectedId(id);
        QCoreApplication::installTranslator(&w.translator);
        const auto uninstall = qScopeGuard([&] { QCoreApplication::removeTranslator(&w.translator); });
        connect(&w.translator, &Translator::changed, &w.backend, &Backend::retranslate);
        connect(&w.translator, &Translator::changed, &w.engine, &QQmlEngine::retranslate);
        QVERIFY(w.open(true));
        auto *repeat = w.window->findChild<QQuickItem *>("repeatRule");
        auto *priority = w.window->findChild<QQuickItem *>("reminderPriority");
        auto *end = w.window->findChild<QQuickItem *>("repeatEnd");
        QVERIFY(repeat && priority && end);
        const auto english = repeat->property("displayText").toString();
        write(w.dir.filePath("language"), "fr\n");
        QTRY_COMPARE(w.translator.language(), QString("fr"));
        QTRY_VERIFY(repeat->property("displayText").toString() != english);
        QTRY_COMPARE(repeat->property("currentIndex").toInt(), 3);
        QTRY_COMPARE(priority->property("currentIndex").toInt(), 3);
        QVERIFY(end->isVisible());
        QCOMPARE(w.store.reminder(id)->repeat, r.repeat);
        QCOMPARE(w.store.reminder(id)->priority, r.priority);
    }

    void theCalendarCanCrossAMonthUsingOnlyKeys() {
        QmlWindow w;
        const auto list = addList(w.store, "Home");
        const auto id = add(w.store, list, "Choose", QDate(2026, 11, 30));
        w.backend.setViewKey("list:" + QString::number(list));
        w.backend.setSelectedId(id);
        QVERIFY(w.open());
        const QVariant dialog(QString("date"));
        QVERIFY(QMetaObject::invokeMethod(w.window, "show", Q_ARG(QVariant, dialog)));
        QTRY_VERIFY(w.window->property("popupOpen").toBool());
        QTest::keyClick(w.window, Qt::Key_Right);
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!w.window->property("popupOpen").toBool());
        QCOMPARE(w.store.reminder(id)->due, QDate(2026, 12, 1));
    }

    void aPendingDateYieldsToNewerValues_data() {
        QTest::addColumn<QString>("change");
        for (const char *change : {"completed", "today-button", "same-day-button", "edited-back", "before-watcher"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void aPendingDateYieldsToNewerValues() {
        QFETCH(QString, change);
        QmlWindow w;
        const auto list = w.store.defaultListId();
        const QDate initial = change == "same-day-button" ? friday.date() : QDate(2031, 1, 15);
        const auto id = add(w.store, list, "Visit", initial);
        auto r = *w.store.reminder(id);
        r.repeat = "daily:1";
        QVERIFY(w.store.put(r));
        Store writer(w.store.path());
        w.backend.setViewKey("list:" + QString::number(list));
        QVERIFY(w.open(true));
        QTest::keyClick(w.window, Qt::Key_D);
        auto *field = w.window->activeFocusItem();
        QVERIFY(field);
        if (change == "edited-back") {
            QTest::keyClick(w.window, Qt::Key_End);
            w.type("8");
            QTest::keyClick(w.window, Qt::Key_Backspace);
        } else {
            w.type("2031-01-14");
        }
        QDate expected;
        if (change.endsWith("button")) {
            auto *button = w.find([](QQuickItem *item) {
                return item->property("compact").toBool() && item->property("text").toString() == "Today";
            });
            QVERIFY(button);
            w.click(button);
            expected = friday.date();
        } else {
            if (change == "completed") {
                expected = writer.complete(id, friday);
            } else {
                r = *writer.reminder(id);
                expected = r.due = QDate(2031, 2, 21);
                QVERIFY(writer.put(r));
            }
            if (change != "before-watcher")
                QVERIFY(w.store.reloadIfChanged());
        }
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(Store(w.store.path()).reminder(id)->due, expected);
        QCOMPARE(field->property("text").toString(), Dates::formatEditableDate(expected));
    }

    void pendingDatesUseTheLanguageTheyWereTypedIn_data() {
        QTest::addColumn<bool>("end");
        QTest::newRow("date") << false;
        QTest::newRow("ends") << true;
    }
    void pendingDatesUseTheLanguageTheyWereTypedIn() {
        QFETCH(bool, end);
        QmlWindow w;
        Reminder r;
        r.listId = w.store.defaultListId();
        r.title = "Visit";
        r.repeat = "weekly:1";
        r.due = QDate(2031, 1, 15);
        r.repeatEnd = QDate(2031, 6, 1);
        const auto id = w.store.put(r);
        w.backend.setViewKey("list:" + QString::number(r.listId));
        QCoreApplication::installTranslator(&w.translator);
        const auto uninstall = qScopeGuard([&] { QCoreApplication::removeTranslator(&w.translator); });
        connect(&w.translator, &Translator::changed, &w.backend, &Backend::retranslate);
        connect(&w.translator, &Translator::changed, &w.engine, &QQmlEngine::retranslate);
        QVERIFY(w.open(true));
        QTest::keyClick(w.window, Qt::Key_D);
        QQuickItem *field = end ? w.window->findChild<QQuickItem *>("repeatEnd") : w.window->activeFocusItem();
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("03/04/2031");
        write(w.dir.filePath("language"), "de\n");
        QTRY_COMPARE(w.translator.language(), QString("de"));
        QCOMPARE(field->property("text").toString(), QString("03/04/2031"));
        QTest::keyClick(w.window, Qt::Key_Return);
        const auto *saved = w.store.reminder(id);
        QCOMPARE(end ? saved->repeatEnd : saved->due, QDate(2031, 3, 4));
        QCOMPARE(field->property("text").toString(), Dates::formatEditableDate(QDate(2031, 3, 4)));
    }

    void aRejectedDraftTitleStaysEditable() {
        QmlWindow w;
        w.backend.setViewKey("list:" + QString::number(w.store.defaultListId()));
        QVERIFY(w.open(true));
        QTest::keyClick(w.window, Qt::Key_N);
        const auto id = w.editingId();
        QVERIFY(id);
        w.backend.setRepeat(id, "daily", 1);
        QVERIFY(w.backend.setRepeatEnd(id, "2031-05-15"));
        auto *editor = w.window->activeFocusItem();
        QVERIFY(editor);
        const QString text = "Order supplies on 2031-06-01";
        w.type(text);
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.editingId(), id);
        QCOMPARE(w.backend.draftId(), id);
        QCOMPARE(w.window->activeFocusItem(), editor);
        QCOMPARE(editor->property("text").toString(), text);
        QCOMPARE(w.store.reminders().size(), 1);
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("Order supplies on 2031-05-01");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.store.reminder(id)->title, QString("Order supplies"));
        QCOMPARE(w.store.reminder(id)->due, QDate(2031, 5, 1));
        QVERIFY(!w.store.reminder(id)->draft);
        QVERIFY(w.editingId() && w.editingId() != id);
    }

    void changingLanguageKeepsTheDateBeingTyped() {
        QmlWindow w;
        const auto list = addList(w.store, "Home"), id = add(w.store, list, "Visit", QDate(2031, 1, 15));
        w.backend.setViewKey("list:" + QString::number(list));
        w.backend.setSelectedId(id);
        QCoreApplication::installTranslator(&w.translator);
        const auto uninstall = qScopeGuard([&] { QCoreApplication::removeTranslator(&w.translator); });
        connect(&w.translator, &Translator::changed, &w.backend, &Backend::retranslate);
        connect(&w.translator, &Translator::changed, &w.engine, &QQmlEngine::retranslate);
        QVERIFY(w.open(true));
        QTest::keyClick(w.window, Qt::Key_D);
        auto *field = w.window->activeFocusItem();
        QVERIFY(field);
        w.type("2031-01-18");
        QCOMPARE(field->property("text").toString(), QString("2031-01-18"));
        write(w.dir.filePath("language"), "de\n");
        QTRY_COMPARE(w.translator.language(), QString("de"));
        QCOMPARE(w.window->activeFocusItem(), field);
        QCOMPARE(field->property("text").toString(), QString("2031-01-18"));
        QCOMPARE(w.store.reminder(id)->due, QDate(2031, 1, 15));
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(Store(w.store.path()).reminder(id)->due, QDate(2031, 1, 18));
    }

    void largeInterleavedModelEditsRebuildInOneStep() {
        QTemporaryDir dir;
        Store store(dir.filePath("reminders.db"));
        ReminderModel model(&store);
        QVector<Row> sparse, full;
        for (qint64 id = 1; id <= 400; ++id) {
            Row r;
            r.id = r.key = id;
            full << r;
            if (id % 2 == 0)
                sparse << r;
        }
        model.show(sparse, true);
        QSignalSpy resets(&model, &ReminderModel::modelReset);
        model.show(full, false);
        QCOMPARE(resets.count(), 1);
        QCOMPARE(model.rowCount(), 400);
        QCOMPARE(model.rowOf(1), 0);
        model.show(sparse, false);
        QCOMPARE(resets.count(), 2);
        QCOMPARE(model.rowOf(1), -1);
        QCOMPARE(model.rowOf(400), 199);
    }

    void savingAListNameKeepsTheColorChangedWhileItsEditorWasOpen() {
        QmlWindow w;
        const qint64 id = addList(w.store, "Work");
        const QString view = "list:" + QString::number(id);
        w.backend.setViewKey(view);
        QVERIFY(w.open());
        const QVariant key(view);
        QVERIFY(QMetaObject::invokeMethod(w.window, "editList", Q_ARG(QVariant, key)));
        QTRY_VERIFY(w.window->property("popupOpen").toBool());
        Store other(w.store.path());
        TaskList recolored = *other.list(id);
        recolored.color = "red";
        QVERIFY(other.put(recolored));
        QVERIFY(w.store.reloadIfChanged());
        w.type("Office");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.store.list(id)->name, QString("Office"));
        QCOMPARE(w.store.list(id)->color, QString("red"));
    }

    void listNamesStayDistinctAndAmbiguityCanBeResolved() {
        QmlWindow w;
        const auto home = w.backend.addList("Home", "blue");
        QVERIFY(w.open());
        QTest::keyClick(w.window, Qt::Key_N, Qt::ShiftModifier);
        QTRY_VERIFY(w.window->property("popupOpen").toBool());
        Store other(w.store.path());
        const auto office = addList(other, "Office");
        QSignalSpy notices(&w.backend, &Backend::notice);
        w.type("Office");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(w.store.lists().size(), 2);
        QVERIFY(w.window->property("popupOpen").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QVERIFY(!w.backend.updateList(home, "Office", "red"));
        QCOMPARE(w.store.list(home)->name, QString("Home"));
        QCOMPARE(w.store.list(home)->color, QString("blue"));

        // An older backup can contain an exact pair. Each name-based command
        // explains how to resolve it, including a delete with --force.
        const auto duplicate = addList(other, "Office");
        const QStringList commands[]{{"list", "--list", "Office"}, {"add", "Call", "--list", "Office"},
                                      {"lists", "delete", "Office", "--force"}};
        for (const auto &command : commands) {
            const auto ran = cliRun(w.store.path(), command);
            QCOMPARE(ran.code, 1);
            QVERIFY(ran.err.contains("More than one list"));
            QVERIFY(ran.err.contains("window"));
        }
        QVERIFY(w.backend.updateList(duplicate, "Studio", {}));
        QCOMPARE(cliRun(w.store.path(), {"add", "Call", "--list", "Office"}).code, 0);
        QCOMPARE(Store(w.store.path()).reminder(1)->listId, office);
    }

    void deleteCompletedAsksWithoutPromisingACount() {
        QmlWindow w;
        const qint64 list = addList(w.store, "Work");
        const qint64 id = add(w.store, list, "Checked");
        w.store.complete(id, friday);
        w.backend.setViewKey("list:" + QString::number(list));
        QVERIFY(w.open());
        QVERIFY(QMetaObject::invokeMethod(w.window, "clearCompleted"));
        QTRY_VERIFY(w.window->property("popupOpen").toBool());
        QVERIFY(w.find([](QQuickItem *item) { return item->property("text").toString() == "Delete completed"; }));
        Store other(w.store.path());
        QVERIFY(other.reopen(id));
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!w.window->property("popupOpen").toBool());
        QVERIFY(w.store.reminder(id));
        QVERIFY(!w.store.reminder(id)->completed);
    }

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
    void openingAnAlertScrollsToTheReminderAfterChangingViews() {
        QmlWindow ui;
        const qint64 listId = addList(ui.store, "Home");
        qint64 target = 0;
        {
            Store::Tx tx(&ui.store);
            for (int i = 0; i < 300; ++i) {
                Reminder r;
                r.listId = listId;
                r.title = QString("Reminder %1").arg(i);
                if (i % 2 == 0)
                    r.notes = "Something to do";
                r.position = i + 1;
                target = ui.store.put(r);
            }
            QVERIFY(tx.commit());
        }
        QVERIFY(ui.open());
        ui.backend.setViewKey("today");
        auto *list = ui.window->findChild<QQuickItem *>("reminders");
        QVERIFY(list);
        auto visible = [&] {
            const QQuickItem *row = ui.row(target);
            if (!row)
                return false;
            const qreal top = row->mapToItem(list, QPointF()).y();
            return top >= -1 && top + row->height() <= list->height() + 1;
        };

        QVERIFY(ui.backend.revealReminder(ui.store.path(), target));
        QCOMPARE(ui.backend.viewListId(), listId);
        QCOMPARE(ui.backend.selectedId(), target);
        QTRY_VERIFY(visible());

        list->setProperty("contentY", list->property("originY"));
        QTRY_VERIFY(!visible());
        QVERIFY(ui.backend.revealReminder(ui.store.path(), target));
        QTRY_VERIFY(visible());
    }

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

    // A picture of a reminder being written (nudge open --type): the window
    // starts a new one with the words in its field, read as they would be
    // had they been typed, and Return keeps it as it reads.
    void aNewReminderCanStartWithWordsTyped() {
        QmlWindow w;
        const qint64 listId = addList(w.store, "Home");
        w.backend.setViewKey("list:" + QString::number(listId));
        QVERIFY(w.open());
        const QString words = "Call the dentist tomorrow at 9 #health";
        QVERIFY(QMetaObject::invokeMethod(w.window, "type", Q_ARG(QVariant, QVariant(words))));
        QTRY_VERIFY(w.editingId() > 0);
        const auto typed = [&w] {
            auto *editor = w.window->property("activeEditor").value<QObject *>();
            return editor ? editor->property("text").toString() : QString();
        };
        QTRY_COMPARE(typed(), words);
        QCOMPARE(w.window->property("typed").toString(), QString());
        const QString reading = w.backend.quickPreview(typed());
        QVERIFY2(reading.startsWith("Tomorrow, 9:00") && reading.endsWith("#health"), qPrintable(reading));

        const qint64 id = w.editingId();
        QTest::keyClick(w.window, Qt::Key_Return);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        const Reminder *kept = w.store.reminder(id);
        QVERIFY(kept);
        QCOMPARE(kept->title, QString("Call the dentist"));
        QCOMPARE(kept->due, friday.date().addDays(1));
        QCOMPARE(kept->dueMinutes, 9 * 60);
        QCOMPARE(kept->tags, QStringList{"health"});
        QCOMPARE(w.store.reminders().size(), 1);
        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
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

        // Alpha and Beta have the same date as the window's clock.
        w.click(date);
        QTRY_VERIFY(date->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("fri");
        w.backend.setSelectedId(b);
        QCOMPARE(w.store.reminder(a)->due, QDate(2026, 11, 20));
        QCOMPARE(date->property("text").toString(), Dates::formatEditableDate(today));
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

private:
    // Clicks a reminder's box once the rows have stopped sliding aside.
    static bool tickBox(const QmlWindow &w, qint64 id) {
        QQuickItem *row = w.row(id);
        for (qreal before = -1; row && row->y() != before;) {
            before = row->y();
            QTest::qWait(60);
        }
        QQuickItem *box = row ? w.find([](QQuickItem *item) {
            return item->inherits("QQuickRectangle") && item->width() == 15 && item->height() == 15;
        }, row) : nullptr;
        if (box)
            w.click(box);
        return box != nullptr;
    }

private slots:
    // A title being typed is kept when its row is taken away while another
    // window has the keyboard (an alert's Open moving the view, the language
    // changing), and the keys work again once the window is back. A row
    // scrolled out of sight while its field has the keyboard is still typed in.
    void aTitleBeingTypedOutlastsItsRow() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home"), work = addList(w.store, "Work");
        const qint64 alpha = add(w.store, home, "Alpha");
        const qint64 report = add(w.store, work, "Report");
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(alpha);
        QVERIFY(w.open());
        auto *list = w.window->findChild<QQuickItem *>("reminders");
        QWindow other;
        other.resize(80, 80);
        // Another window takes the keyboard, the list changes meanwhile, and
        // the window comes back: what an alert's Open does, bar hyprctl.
        const auto away = [&](const std::function<void()> &meanwhile) {
            other.show();
            other.requestActivate();
            if (!QTest::qWaitForWindowActive(&other) || w.window->isActive())
                return false;
            meanwhile();
            w.window->requestActivate();
            return QTest::qWaitForWindowActive(w.window);
        };

        // A new reminder, and the view moves to another list.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        w.type("Water the plants");
        QVERIFY(away([&] {
            w.backend.setViewKey("list:" + QString::number(work));
            w.backend.setSelectedId(report);
        }));
        QCOMPARE(w.editingId(), 0);
        QVERIFY(w.titled("Water the plants"));
        QVERIFY(w.window->property("keys").toBool());

        // A rename, and the language changes.
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(alpha);
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), alpha);
        w.type(" and more");
        QVERIFY(away([&] { w.backend.retranslate(); }));
        QCOMPARE(w.editingId(), 0);
        QCOMPARE(w.store.reminder(alpha)->title, QString("Alpha and more"));
        QVERIFY(w.window->property("keys").toBool());

        // A new reminder, and a reminder far down the same list is shown.
        for (int i = 0; i < 60; ++i)
            add(w.store, home, QString("Item %1").arg(i));
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 tea = w.editingId();
        w.type("Tea");
        QVERIFY(away([&] { w.backend.setSelectedId(w.titled("Item 59")); }));
        QVERIFY(!w.row(tea));
        QCOMPARE(w.editingId(), 0);
        QCOMPARE(w.titled("Tea"), tea);
        QVERIFY(w.window->property("keys").toBool());

        // Scrolled away while it has the keyboard, the typing goes on.
        w.backend.setSelectedId(alpha);
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 green = w.editingId();
        w.type("Green");
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        QTRY_VERIFY(!w.row(green));
        QCOMPARE(w.editingId(), green);
        w.type(" tea");
        QMetaObject::invokeMethod(list, "positionViewAtBeginning");
        QTRY_VERIFY(w.row(green));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        QCOMPARE(w.titled("Green tea"), green);
        QVERIFY(!w.store.reminder(green)->completed);

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // Ticking a box, or a section's tool, while a new reminder is being typed
    // finishes its title first: adding and naming it stay one step to undo,
    // and one left blank goes without a trace.
    void aClickOnABoxFinishesTheTitleFirst() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const qint64 a = add(w.store, home, "Alpha"), b = add(w.store, home, "Beta");
        Section section;
        section.listId = home;
        section.name = "Later";
        section.position = 1;
        const qint64 later = w.store.put(section);
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(a);
        w.store.clearUndo();
        QVERIFY(w.open());

        // Another reminder's box.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 milk = w.editingId();
        w.type("Buy milk");
        QVERIFY(tickBox(w, b));
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(w.store.reminder(b)->completed);
        QCOMPARE(w.titled("Buy milk"), milk);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(b)->completed);
        QCOMPARE(w.titled("Buy milk"), milk);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(milk));
        QVERIFY(!w.store.canUndo());

        // Left blank: another reminder's box, then its own.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 blank = w.editingId();
        QVERIFY(tickBox(w, a));
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(!w.store.reminder(blank));
        QVERIFY(w.store.reminder(a)->completed);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(a)->completed);
        QCOMPARE(w.store.reminders().size(), 2);
        QVERIFY(!w.store.canUndo());
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 own = w.editingId();
        QVERIFY(tickBox(w, own));
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(!w.store.reminder(own));
        QVERIFY(w.store.reminder(w.backend.selectedId())); // nothing gone stays selected
        QVERIFY(w.backend.selectedRow() >= 0);
        QVERIFY(!w.store.canUndo());

        // A section's trash.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 mum = w.editingId();
        w.type("Call mum");
        QQuickItem *heading = w.find([later](QQuickItem *item) { return item->property("sectionId").toLongLong() == later; });
        QVERIFY(heading);
        QTest::mouseMove(w.window, heading->mapToScene(QPointF(heading->width() / 2, heading->height() / 2)).toPoint());
        QQuickItem *trash = w.find([](QQuickItem *item) { return item->property("name").toString() == "trash"; }, heading);
        QVERIFY(trash);
        QTRY_VERIFY(trash->isVisible() && trash->width() > 0);
        w.click(trash);
        QTRY_VERIFY(w.store.sections().isEmpty());
        QCOMPARE(w.editingId(), 0);
        QCOMPARE(w.titled("Call mum"), mum);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(w.store.sections().size(), 1);
        QCOMPARE(w.titled("Call mum"), mum);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(mum));
        QVERIFY(!w.store.canUndo());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // A new reminder left for the details before it has a title is the one
    // they fill in: what is typed there lands on it, never on the reminder
    // selected before it. Clicked away from, it stays if anything was typed in
    // it, as one step to undo, and goes without a trace if nothing was.
    void aNewReminderIsFilledInThroughTheDetails() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const qint64 a = add(w.store, home, "Alpha"), b = add(w.store, home, "Beta"), c = add(w.store, home, "Gamma");
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(b);
        w.store.clearUndo();
        QVERIFY(w.open(true));
        QQuickItem *title = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("text").toString() == "Beta";
        });
        QQuickItem *notes = w.find([](QQuickItem *item) { return item->inherits("QQuickTextArea"); });
        QQuickItem *date = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("placeholderText").toString() == "Date";
        });
        QVERIFY(title && notes && date);
        const auto others = [&w, a, b, c] {
            QStringList seen;
            for (qint64 id : {a, b, c}) {
                const Reminder *r = w.store.reminder(id);
                seen << QString("%1|%2|%3").arg(r->title, r->notes, r->due.toString(Qt::ISODate));
            }
            return seen;
        };
        const QStringList before = others();

        // Its title.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 milk = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QCOMPARE(w.editingId(), 0);
        QCOMPARE(w.backend.selectedId(), milk);
        QCOMPARE(title->property("text").toString(), QString());
        w.type("Buy milk");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.titled("Buy milk"), milk);
        QCOMPARE(others(), before);
        w.clickRow(c);
        QCOMPARE(w.backend.selectedId(), c);
        QCOMPARE(w.backend.draftId(), 0);
        QCOMPARE(w.titled("Buy milk"), milk);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(milk));
        QVERIFY(!w.store.canUndo());

        // Its title typed but not entered, and the selection moves on with no
        // click, as an alert's Show moves it.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 eggs = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        w.type("Eggs");
        w.backend.setSelectedId(a);
        QCOMPARE(w.titled("Eggs"), eggs);
        QCOMPARE(w.backend.draftId(), 0);
        QCOMPARE(others(), before);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!title->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(eggs));
        QVERIFY(!w.store.canUndo());

        // Its notes and its date, and no title: what was typed stays.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 cash = w.editingId();
        w.click(notes);
        QTRY_VERIFY(notes->hasActiveFocus());
        QCOMPARE(w.backend.selectedId(), cash);
        w.type("Bring cash");
        w.click(date);
        QTRY_VERIFY(date->hasActiveFocus());
        w.type("tomorrow");
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.store.reminder(cash));
        QCOMPARE(w.store.reminder(cash)->notes, QString("Bring cash"));
        QCOMPARE(w.store.reminder(cash)->due, QDate(2026, 11, 14));
        QCOMPARE(w.backend.selectedId(), cash);
        QCOMPARE(others(), before);
        // Renamed in the list with nothing typed, it keeps what it was given.
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), cash);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        QVERIFY(w.store.reminder(cash));
        QCOMPARE(w.backend.selectedId(), cash);
        w.clickRow(a);
        QCOMPARE(w.backend.selectedId(), a);
        QCOMPARE(w.backend.draftId(), 0);
        QVERIFY(w.store.reminder(cash));
        QCOMPARE(w.store.reminder(cash)->notes, QString("Bring cash"));
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(cash));
        QVERIFY(!w.store.canUndo());

        // Nothing typed in it, and left for another new reminder, deleted, or
        // left for another reminder.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 first = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QCOMPARE(w.backend.selectedId(), first);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        QVERIFY(!w.store.reminder(first));
        const qint64 second = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_Delete);
        QVERIFY(!w.store.reminder(second));
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 third = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        w.clickRow(b);
        QCOMPARE(w.backend.selectedId(), b);
        QVERIFY(!w.store.reminder(third));
        QCOMPARE(w.store.reminders().size(), 3);
        QVERIFY(!w.store.canUndo());
        QCOMPARE(others(), before);
        // Or for another reminder's box: only the tick is left to undo.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 fourth = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QVERIFY(tickBox(w, c));
        QVERIFY(!w.store.reminder(fourth));
        QVERIFY(w.store.reminder(c)->completed);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(c)->completed);
        QCOMPARE(w.store.reminders().size(), 3);
        QVERIFY(!w.store.canUndo());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // A new reminder the details are filling in stays selected when what they
    // give it takes it out of the view, so the next field typed in still fills
    // it in. A choice from a dropdown is made for it too.
    void aNewReminderStaysWithTheDetailsFillingItIn() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home"), work = addList(w.store, "Work");
        const QDate today = friday.date();
        const qint64 a = add(w.store, home, "Alpha", today), b = add(w.store, home, "Beta", today);
        w.backend.setViewKey("today");
        w.backend.setSelectedId(b);
        w.store.clearUndo();
        QVERIFY(w.open(true));
        QQuickItem *title = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("text").toString() == "Beta";
        });
        QQuickItem *date = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("placeholderText").toString() == "Date";
        });
        QQuickItem *lists = w.find([](QQuickItem *item) {
            return item->inherits("QQuickComboBox") && item->property("textRole").toString() == "name";
        });
        QQuickItem *priority = w.find([](QQuickItem *item) { return isDropdown(item, 4); });
        QVERIFY(title && date && lists && priority);

        // A date that takes it out of Today, then its title.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 dentist = w.editingId();
        w.click(date);
        QTRY_VERIFY(date->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("tomorrow");
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(w.store.reminder(dentist));
        QCOMPARE(w.store.reminder(dentist)->due, QDate(2026, 11, 14));
        QCOMPARE(w.backend.selectedRow(), -1);
        QCOMPARE(w.backend.selectedId(), dentist);
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QCOMPARE(title->property("text").toString(), QString());
        w.type("Dentist");
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(w.titled("Dentist"), dentist);
        QCOMPARE(w.store.reminder(b)->title, QString("Beta"));
        QCOMPARE(w.store.reminder(b)->due, today);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_J);
        QVERIFY(w.backend.selectedRow() >= 0);
        QCOMPARE(w.backend.draftId(), 0);
        QCOMPARE(w.titled("Dentist"), dentist);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(dentist));
        QVERIFY(!w.store.canUndo());

        // A priority chosen, then its title typed in the list.
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 rent = w.editingId();
        QVERIFY(w.pick(priority, 3));
        QVERIFY(w.store.reminder(rent));
        QCOMPARE(w.store.reminder(rent)->priority, 3);
        QCOMPARE(w.backend.selectedId(), rent);
        QCOMPARE(w.store.reminder(a)->priority, 0);
        QCOMPARE(w.store.reminder(b)->priority, 0);
        QTRY_VERIFY(w.window->property("keys").toBool());
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.editingId(), rent);
        w.type("Pay rent");
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_COMPARE(w.editingId(), 0);
        QCOMPARE(w.titled("Pay rent"), rent);
        QCOMPARE(w.backend.draftId(), 0);
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.store.reminder(rent));
        QVERIFY(!w.store.canUndo());

        // Sent to another list, out of the view, and left with nothing typed
        // in it when the view moves on: it goes without a trace.
        w.backend.setViewKey("list:" + QString::number(home));
        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 blank = w.editingId();
        QVERIFY(w.pick(lists, 1));
        QVERIFY(w.store.reminder(blank));
        QCOMPARE(w.store.reminder(blank)->listId, work);
        QCOMPARE(w.backend.selectedId(), blank);
        QCOMPARE(w.backend.selectedRow(), -1);
        w.backend.setViewKey("flagged");
        QVERIFY(!w.store.reminder(blank));
        QCOMPARE(w.store.reminders().size(), 2);
        QVERIFY(!w.store.canUndo());

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    // A repeat number typed but not entered is saved to the reminder it was
    // typed for when the selection moves on with no click, as an alert's Show
    // moves it, and the next reminder shows its own, even one that reads the same.
    void aRepeatTypedIsSavedToItsOwnReminder() {
        QmlWindow w;
        const qint64 home = addList(w.store, "Home");
        const qint64 b = add(w.store, home, "Beta", friday.date()), c = add(w.store, home, "Gamma", friday.date());
        for (const qint64 id : {b, c}) {
            Reminder r = *w.store.reminder(id);
            r.repeat = "weekly:1";
            w.store.put(r);
        }
        w.backend.setViewKey("list:" + QString::number(home));
        w.backend.setSelectedId(b);
        QVERIFY(w.open(true));
        QQuickItem *every = w.find([](QQuickItem *item) { return item->inherits("QQuickSpinBox"); });
        QVERIFY(every);
        auto *number = every->property("contentItem").value<QQuickItem *>();
        QVERIFY(number);
        QTRY_COMPARE(number->property("text").toString(), QString("1"));

        w.click(number);
        QTRY_VERIFY(number->hasActiveFocus());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("5");
        w.backend.setSelectedId(c);
        QCOMPARE(w.store.reminder(b)->repeat, QString("weekly:5"));
        QCOMPARE(w.store.reminder(c)->repeat, QString("weekly:1"));
        QCOMPARE(number->property("text").toString(), QString("1"));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!number->hasActiveFocus());
        QCOMPARE(w.store.reminder(b)->repeat, QString("weekly:5"));
        QCOMPARE(w.store.reminder(c)->repeat, QString("weekly:1"));

        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join('\n')));
    }

    void typingForANewReminderOutlastsAnAlertsOpen_data() {
        QTest::addColumn<bool>("sections");
        QTest::newRow("its list's first row by sections") << true;
        QTest::newRow("its list's first row by title") << false;
    }

    // Words typed in the details for a new reminder, not yet entered, when an
    // alert's Open shows the list the new reminder now heads.
    void typingForANewReminderOutlastsAnAlertsOpen() {
        QFETCH(bool, sections);
        QmlWindow w;
        const qint64 home = addList(w.store, "Home"), work = addList(w.store, "Work");
        add(w.store, home, "Alpha", friday.date());
        const qint64 beta = add(w.store, work, "Beta", friday.date(), 15 * 60);
        const qint64 report = add(w.store, work, "Report", friday.date().addDays(3), 9 * 60);
        if (sections) {
            Section later;
            later.listId = work;
            later.name = "Later";
            later.position = 1;
            const qint64 laterId = w.store.put(later);
            for (const qint64 id : {beta, report}) {
                Reminder r = *w.store.reminder(id);
                r.sectionId = laterId;
                w.store.put(r);
            }
        } else {
            w.backend.setViewKey("list:" + QString::number(work));
            w.backend.setSortMode("title");
        }
        w.backend.setViewKey("today");
        w.backend.setSelectedId(beta);
        w.store.clearUndo();
        QVERIFY(w.open(true));
        QQuickItem *title = w.find([](QQuickItem *item) {
            return item->inherits("QQuickTextField") && item->property("text").toString() == "Beta";
        });
        QVERIFY(title);

        QTest::keyClick(w.window, Qt::Key_N);
        QTRY_VERIFY(w.editingId() > 0);
        const qint64 draft = w.editingId();
        w.click(title);
        QTRY_VERIFY(title->hasActiveFocus());
        QCOMPARE(w.backend.draftId(), draft);
        w.type("Call the bank");
        QVERIFY(w.window->isActive());

        // What Backend::reveal does for the alert, without asking Hyprland.
        w.backend.setViewKey("list:" + QString::number(work));
        w.backend.setSelectedId(report);
        QTRY_VERIFY(w.store.reminder(draft));
        QCOMPARE(w.store.reminder(draft)->title, QString("Call the bank"));
        QCOMPARE(w.store.reminder(beta)->title, QString("Beta"));
        // Added and named in one step.
        w.backend.undo();
        QVERIFY(!w.store.reminder(draft));
        QVERIFY(!w.store.canUndo());
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

    // A picture can ask for a theme by name (nudge open --theme). It is
    // found among the themes, the person's own before Omarchy's, and a name
    // is never a way out of them; a folder is given by its path.
    void aThemeAskedForIsFoundAmongTheThemes() {
        QTemporaryDir dir;
        const QDir root(dir.path());
        for (const QString &theme : QStringList{"own/nord", "shipped/nord", "shipped/gruvbox"}) {
            QVERIFY(root.mkpath(theme));
            QFile colors(root.filePath(theme + "/colors.toml"));
            QVERIFY(colors.open(QIODevice::WriteOnly));
        }
        const QStringList kept{root.filePath("own"), root.filePath("shipped")};
        QCOMPARE(Look::themeFolder("nord", kept), root.filePath("own/nord"));
        QCOMPARE(Look::themeFolder("gruvbox", kept), root.filePath("shipped/gruvbox"));
        QCOMPARE(Look::themeFolder(root.filePath("shipped/nord"), kept), root.filePath("shipped/nord"));
        QCOMPARE(Look::themeFolder("rose-pine", kept), QString());
        QCOMPARE(Look::themeFolder("", kept), QString());
        QCOMPARE(Look::themeFolder("..", kept), QString());
        QCOMPARE(Look::themeFolder("own", {root.path()}), QString()); // a folder of themes is not a theme
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
        QCOMPARE(shown("fil", "Its %Ln reminders are deleted with it. Undo brings them back.", 2),
                 QString("Its 2 reminders are deleted with it. Undo brings them back."));
        QCOMPARE(shown("ja", "Deleted %n completed reminders", 1), QString("Deleted 1 completed reminder"));
        QCOMPARE(shown("ru", "Every %n days", 21), QString("Every 21 days"));
        // A language with a catalog keeps its own rules: French counts 0 as one.
        QCOMPARE(shown("fr", "Deleted %n completed reminders", 0), QString("0 rappel terminé supprimé"));
    }

    void listDeletionCountsUseLocalDigitGrouping() {
        QCOMPARE(shown("en", "Its %Ln reminders are deleted with it. Undo brings them back.", 3600),
                 QString("Its 3,600 reminders are deleted with it. Undo brings them back."));
    }

    void aLanguageSelectedAfterItsDirectoryIsMadeIsPickedUp() {
        QTemporaryDir dir;
        const QString language = dir.filePath("config/omarchy/language");
        Translator translator(language);
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(QDir().mkpath(QFileInfo(language).absolutePath()));
        write(language, "de\n");
        QTRY_COMPARE(translator.language(), QString("de"));
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

    // Every translated string has an entry in each non-English catalog.
    // English can use the source text for ordinary, uncounted strings.
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
        static const QRegularExpression placeholder("%L?(n|\\d)");

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
                    const QJsonObject translated = it.value().toObject();
                    const QString locale = name.chopped(5);
                    if (Translator::pluralCategory(locale, 1000000) == "many") {
                        QVERIFY2(translated.contains("many"), qPrintable(where + " has no many form"));
                        forms << translated.value("many").toString();
                    }
                    forms << translated.value("other").toString();
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
