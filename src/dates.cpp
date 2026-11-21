#include "dates.h"

#include "recurrence.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVector>
#include <algorithm>

namespace {

int weekday(const QString &word) {
    static const QHash<QString, int> full{{"monday", 1}, {"tuesday", 2},  {"wednesday", 3}, {"thursday", 4},
                                          {"friday", 5}, {"saturday", 6}, {"sunday", 7}};
    static const QHash<QString, int> shortened{{"mon", 1},  {"tue", 2},   {"tues", 2}, {"wed", 3}, {"thu", 4},
                                               {"thur", 4}, {"thurs", 4}, {"fri", 5},  {"sat", 6}, {"sun", 7}};
    if (const int day = full.value(word))
        return day;
    return shortened.value(word);
}

int month(const QString &word) {
    static const QStringList names{"january", "february", "march",     "april",   "may",      "june",
                                   "july",    "august",   "september", "october", "november", "december"};
    for (int i = 0; i < names.size(); ++i)
        if (word == names[i] || word == names[i].left(3) || (i == 8 && word == "sept"))
            return i + 1;
    return 0;
}

// "5", "5th", "21st": a day of the month, or 0.
int dayNumber(const QString &word) {
    static const QRegularExpression pattern("^(\\d{1,2})(st|nd|rd|th)?$");
    const auto match = pattern.match(word);
    const int day = match.hasMatch() ? match.captured(1).toInt() : 0;
    return day >= 1 && day <= 31 ? day : 0;
}

int year(const QString &word) {
    static const QRegularExpression pattern("^(\\d{4})$");
    const auto match = pattern.match(word);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

// The first day from today on with this month and day. The 29th of February
// waits for a leap year, which is never more than eight years off.
QDate nextDate(int month, int day, const QDate &today) {
    for (int year = today.year(); year <= qMin(9999, today.year() + 8); ++year)
        if (const QDate found(year, month, day); found.isValid() && found >= today)
            return found;
    return {};
}

QString editablePattern(const QLocale &locale) {
    static const QRegularExpression shortYear("(?<!y)yy(?!y)");
    return locale.dateFormat(QLocale::ShortFormat).replace(shortYear, "yyyy");
}

QDate numericDate(const QString &text, const QDate &today, const QLocale &locale) {
    const QString written = locale.dateFormat(QLocale::ShortFormat);
    const bool dayFirst = written.indexOf('d') < written.indexOf('M') || written.startsWith('y');
    const QStringList patterns = dayFirst
        ? QStringList{"d/M/yyyy", "d.M.yyyy", "d-M-yyyy", "d/M/yy", "d.M.yy", "d/M", "d.M", "d.M."}
        : QStringList{"M/d/yyyy", "M-d-yyyy", "M/d/yy", "M/d"};
    for (const QString &pattern : patterns) {
        const bool dated = pattern.contains('y');
        // Use a leap year for yearless dates and a separate pivot for yy.
        const QString input = dated ? text : text + "/2000";
        const QString format = dated ? pattern : pattern + "/yyyy";
        const QDate found = locale.toDate(input, format, 1970);
        if (found.isValid())
            return dated ? found : nextDate(found.month(), found.day(), today);
    }
    return {};
}

// A count as people write it: "3", "a", "two".
int count(const QString &word) {
    static const QHash<QString, int> words{{"a", 1},    {"an", 1},  {"one", 1},   {"two", 2},   {"three", 3}, {"four", 4},
                                           {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9},  {"ten", 10}};
    bool ok = false;
    const int number = word.toInt(&ok);
    if (ok)
        return number > 0 && number < 1000 ? number : 0;
    return words.value(word);
}

struct Time {
    int minutes = -1;
    bool bare = false; // just an hour, with nothing to say it is a time
    int used = 0;
    bool guessed = false; // morning or afternoon was a guess: "at 9", "at 5:30"
};

// Whether the locale writes its times with am and pm. Quoted text in the
// format is written as it stands, so an "a" there does not count.
bool twelveHourClock(const QLocale &locale) {
    static const QRegularExpression quoted("'[^']*'");
    return locale.timeFormat(QLocale::ShortFormat).remove(quoted).contains(u'a', Qt::CaseInsensitive);
}

Time timeAt(const QStringList &words, int index, const QLocale &locale) {
    const QString word = words.value(index);
    if (word == "noon" || word == "midday")
        return {12 * 60, false, 1};
    if (word == "midnight")
        return {0, false, 1};

    static const QRegularExpression pattern("^(\\d{1,2})(?:([:.])(\\d{2}))?(am|pm|a|p|a\\.m\\.|p\\.m\\.)?$");
    const auto match = pattern.match(word);
    if (!match.hasMatch())
        return {};
    int hour = match.captured(1).toInt();
    const bool hasMinutes = !match.captured(3).isEmpty();
    const int minute = match.captured(3).toInt();
    QString half = match.captured(4);
    int used = 1;
    const bool onTheHour = !hasMinutes && (words.value(index + 1) == "o'clock" || words.value(index + 1) == "o’clock");
    if (onTheHour)
        ++used;
    if (half.isEmpty()) {
        const QString next = words.value(index + used);
        if (next == "am" || next == "pm" || next == "a.m." || next == "p.m.") {
            half = next;
            ++used;
        }
    }
    // "5.30" is more often a price than a time, unless it says am or pm.
    if (match.captured(2) == "." && half.isEmpty())
        return {};
    if (minute > 59)
        return {};

    if (!half.isEmpty()) {
        if (hour < 1 || hour > 12)
            return {};
        hour = hour % 12 + (half.startsWith('p') ? 12 : 0);
        // "2a" is a room, a seat, or a step as often as two in the morning,
        // so it takes an "at" to be a time. "7p" is the evening.
        return {hour * 60 + minute, half == "a" && !hasMinutes, used};
    }
    if (hour > 23)
        return {};
    // "at 5" means the afternoon and "at 9" the morning. Where the clock has
    // twelve hours "at 5:30" is the afternoon too; where it has 24, 5:30 is
    // how the morning is written. An hour like 0, 17, or 05 is the 24-hour
    // clock's and means what it says.
    const bool guessed = hour >= 1 && hour <= 12 && !match.captured(1).startsWith('0');
    if (guessed && hour <= 7 && (!hasMinutes || twelveHourClock(locale)))
        hour += 12;
    return {hour * 60 + minute, !hasMinutes && !onTheHour, used, guessed};
}

struct Parsed {
    QuickEntry entry;
    int leftover = 0; // words that were not understood
};

Parsed parse(const QString &text, const QDateTime &now, bool lenient, const QLocale &locale = QLocale()) {
    static const QRegularExpression space("\\s+");
    const QStringList tokens = text.split(space, Qt::SkipEmptyParts);
    QStringList words;
    for (const QString &token : tokens) {
        QString word = token.toLower();
        while (word.size() > 1 && (word.endsWith(',') || word.endsWith(';') || word.endsWith('?') ||
                                   (word.endsWith('.') && !word.endsWith("a.m.") && !word.endsWith("p.m."))))
            word.chop(1);
        words << word;
    }
    const int size = words.size();
    QVector<bool> used(size, false);
    const QDate today = now.date();

    QDate date;
    int minutes = -1;
    bool guessed = false; // the time's morning or afternoon was a guess
    bool lateFold = false;
    Repeat repeat;
    QDate repeatEnd;
    int repeatDay = 0; // "every friday"
    bool tonight = false;
    int dayPart = -1;
    QStringList tags;
    int priority = 0;

    // A date phrase takes the little word that led up to it along, where the
    // word belongs to it: "due in 3 days", "by tomorrow", "on nov 20", "this
    // friday". Elsewhere it is the title's: "Finish this tomorrow". A date
    // field has no title, so there any of them goes.
    static const QStringList beforeAny{"due"}, beforeDay{"due", "by", "until"},
        beforeDate{"due", "by", "until", "on"}, beforeWeekday{"due", "by", "until", "on", "this"};
    auto take = [&](int from, int length, const QStringList &leading) {
        for (int k = 0; k < length; ++k)
            used[from + k] = true;
        if (from > 0 && !used[from - 1] && (lenient ? beforeWeekday : leading).contains(words[from - 1]))
            used[from - 1] = true;
    };
    // Bare weekdays and named times count where little else can follow.
    auto closes = [&](int index) {
        if (index >= size)
            return true;
        const QString &word = words[index];
        if (word == "at" || word == "every" || word.startsWith('#') || word == "!" || word == "!!" || word == "!!!")
            return true;
        const Time time = timeAt(words, index, locale);
        return time.minutes >= 0 && !time.bare;
    };
    auto qualifier = [](const QString &word) {
        static const QStringList parts{"morning", "afternoon", "evening", "night"};
        return parts.indexOf(word);
    };
    auto dayLength = [&](int from, int length) {
        return length + (qualifier(words.value(from + length)) >= 0);
    };
    auto takeDay = [&](int from, int length, const QStringList &leading) {
        dayPart = qualifier(words.value(from + length));
        take(from, dayLength(from, length), leading);
    };
    auto article = [&](int index) {
        const QString word = words.value(index - 1);
        return word == "the" || word == "a" || word == "an";
    };
    auto endpointLength = [&](int index) {
        const QString word = words.value(index);
        if (word == "next" && weekday(words.value(index + 1)))
            return dayLength(index, 2);
        if (weekday(word) || word == "tomorrow" || word == "today" || QDate::fromString(word, Qt::ISODate).isValid())
            return dayLength(index, 1);
        return timeAt(words, index, locale).used;
    };
    // Keep a range or an ordinal weekday rule together if it cannot be
    // scheduled as written. Pulling out one date changes its meaning.
    static const QRegularExpression ordinal("^(?:first|second|third|fourth|fifth|last|\\d+(?:st|nd|rd|th))$");
    bool keepSchedule = false;
    for (int i = 0; !lenient && i + 2 < size; ++i) {
        const int length = endpointLength(i);
        const QString join = words.value(i + length);
        if ((length && (join == "to" || join == "through" || join == "or") && endpointLength(i + length + 1)) ||
            (words[i] == "every" && ordinal.match(words[i + 1]).hasMatch() && weekday(words[i + 2]))) {
            keepSchedule = true;
            break;
        }
    }

    for (int i = 0; i < size; ++i) {
        if (used[i])
            continue;
        const QString &word = words[i];
        const QString next = words.value(i + 1), third = words.value(i + 2);

        if (word.size() > 1 && word.startsWith('#')) {
            QString tag = tokens[i].mid(1);
            while (!tag.isEmpty() && !tag.back().isLetterOrNumber() && !tag.endsWith('+') && !tag.endsWith('#'))
                tag.chop(1);
            if (std::any_of(tag.cbegin(), tag.cend(), [](QChar ch) { return ch.isLetterOrNumber(); })) {
                if (!tags.contains(tag, Qt::CaseInsensitive))
                    tags << tag;
                used[i] = true;
                continue;
            }
        }
        if (word == "!" || word == "!!" || word == "!!!") {
            priority = qMax(priority, int(word.size()));
            used[i] = true;
            continue;
        }
        if (keepSchedule)
            continue;

        if (word == "until" && repeat.repeats()) {
            for (int n = qMin(4, size - i - 1); n > 0; --n) {
                repeatEnd = Dates::parseDate(tokens.mid(i + 1, n).join(' '), today, locale);
                if (repeatEnd.isValid()) {
                    take(i, n + 1, {});
                    break;
                }
            }
            if (repeatEnd.isValid())
                continue;
        }

        if (word == "every" && i + 1 < size) {
            int interval = 1, at = i + 1;
            if (next == "other") {
                interval = 2;
                ++at;
            } else if (count(next) > 0 && next != "a" && next != "an" && i + 2 < size) {
                interval = count(next);
                ++at;
            }
            const QString unit = words.value(at);
            Repeat::Unit found = Repeat::Never;
            if (unit == "day" || unit == "days")
                found = Repeat::Daily;
            else if (unit == "week" || unit == "weeks")
                found = Repeat::Weekly;
            else if (unit == "month" || unit == "months")
                found = Repeat::Monthly;
            else if (unit == "year" || unit == "years")
                found = Repeat::Yearly;
            else if ((unit == "weekday" || unit == "weekdays") && interval == 1)
                found = Repeat::Weekdays;
            if (found != Repeat::Never) {
                repeat.unit = found;
                repeat.interval = interval;
                take(i, at - i + 1, beforeAny);
                continue;
            }
            if (const int day = weekday(unit); day && interval <= 2) {
                repeat.unit = Repeat::Weekly;
                repeat.interval = interval;
                repeatDay = day;
                takeDay(i, at - i + 1, beforeAny);
                continue;
            }
        }

        if (word == "today") {
            date = today;
            takeDay(i, 1, beforeDay);
            continue;
        }
        if (word == "tomorrow" || word == "tmrw" || word == "tmr") {
            date = today.addDays(1);
            takeDay(i, 1, beforeDay);
            continue;
        }
        if (word == "tonight" || (word == "this" && next == "evening")) {
            date = today;
            tonight = true;
            take(i, word == "this" ? 2 : 1, beforeDay);
            continue;
        }

        if (word == "next" && i + 1 < size) {
            const QDate monday = today.addDays(8 - today.dayOfWeek());
            if (next == "week") {
                date = monday;
                takeDay(i, 2, beforeDay);
                continue;
            }
            if (next == "month") {
                date = QDate(today.year(), today.month(), 1).addMonths(1);
                takeDay(i, 2, beforeDay);
                continue;
            }
            if (const int day = weekday(next)) {
                date = monday.addDays(day - 1);
                takeDay(i, 2, beforeDate);
                continue;
            }
        }
        if (word == "this" && next == "weekend") {
            date = today.addDays((6 - today.dayOfWeek() + 7) % 7);
            takeDay(i, 2, beforeDay);
            continue;
        }

        if (word == "in" && i + 2 < size && count(next) > 0) {
            const int amount = count(next);
            const QString &unit = third;
            QDate target;
            QDateTime moment;
            if (unit == "day" || unit == "days")
                target = today.addDays(amount);
            else if (unit == "week" || unit == "weeks")
                target = today.addDays(7 * amount);
            else if (unit == "month" || unit == "months")
                target = today.addMonths(amount);
            else if (unit == "year" || unit == "years")
                target = today.addYears(amount);
            else if (unit == "hour" || unit == "hours" || unit == "hr" || unit == "hrs")
                moment = now.addSecs(3600LL * amount);
            else if (unit == "minute" || unit == "minutes" || unit == "min" || unit == "mins")
                moment = now.addSecs(60LL * amount);
            if (moment.isValid()) {
                date = moment.date();
                minutes = moment.time().hour() * 60 + moment.time().minute();
                const QDateTime first(date, moment.time(), moment.timeRepresentation(),
                                      QDateTime::TransitionResolution::PreferBefore);
                lateFold = first.isValid() && first < moment;
                guessed = false;
                take(i, 3, beforeAny);
                continue;
            }
            if (target.isValid()) {
                date = target;
                takeDay(i, 3, beforeAny);
                continue;
            }
        }

        if (const int day = weekday(word)) {
            if (lenient || beforeWeekday.contains(words.value(i - 1)) ||
                (!article(i) && closes(i + dayLength(i, 1)))) {
                int ahead = (day - today.dayOfWeek() + 7) % 7;
                if (ahead == 0 && words.value(i - 1) != "this")
                    ahead = 7;
                date = today.addDays(ahead);
                takeDay(i, 1, beforeWeekday);
                continue;
            }
        }

        // "nov 20", "20 nov", with a year or without.
        int monthNumber = 0, day = 0, length = 0;
        if (month(word) && dayNumber(next)) {
            monthNumber = month(word);
            day = dayNumber(next);
            length = 2;
        } else if (dayNumber(word) && month(next)) {
            monthNumber = month(next);
            day = dayNumber(word);
            length = 2;
        }
        if (length) {
            int stated = year(words.value(i + 2));
            // In a title a bare number could be an amount. A comma makes
            // unusual years explicit, as does typing the date on its own.
            if (!lenient && !tokens[i + 1].endsWith(',') && (stated < 1900 || stated >= 2200))
                stated = 0;
            const QDate found = stated ? QDate(stated, monthNumber, day) : nextDate(monthNumber, day, today);
            if (found.isValid()) {
                date = found;
                takeDay(i, stated ? 3 : 2, beforeDate);
                continue;
            }
        }
        if (word.size() == 10) {
            const QDate iso = QDate::fromString(word, Qt::ISODate);
            if (iso.isValid()) {
                date = iso;
                takeDay(i, 1, beforeDate);
                continue;
            }
        }
        // A fraction or a version number can look like a short date. In a
        // title, dates written this way need a cue such as "on" or "due".
        if ((word.contains('/') || word.contains('.')) && (lenient || beforeDate.contains(words.value(i - 1)))) {
            if (const QDate local = numericDate(word, today, locale); local.isValid()) {
                date = local;
                takeDay(i, 1, beforeDate);
                continue;
            }
        }

        if (word == "at" && i + 1 < size) {
            const Time time = timeAt(words, i + 1, locale);
            if (time.minutes >= 0) {
                minutes = time.minutes;
                guessed = time.guessed;
                lateFold = false;
                used[i] = true;
                for (int k = 0; k < time.used; ++k)
                    used[i + 1 + k] = true;
                continue;
            }
        }
        const Time time = timeAt(words, i, locale);
        const bool clockWord = word == "noon" || word == "midday" || word == "midnight";
        if (time.minutes >= 0 && (lenient || (!time.bare && (!clockWord || (!article(i) && closes(i + 1)))))) {
            minutes = time.minutes;
            guessed = time.guessed;
            lateFold = false;
            for (int k = 0; k < time.used; ++k)
                used[i + k] = true;
            continue;
        }
    }

    const int nowMinutes = now.time().hour() * 60 + now.time().minute();
    if (dayPart >= 0) {
        if (minutes < 0) {
            static const int hours[]{9, 15, 18, 21};
            minutes = hours[dayPart] * 60;
        } else if (guessed) {
            const int hour = minutes / 60 % 12;
            const bool nextMorning = dayPart == 3 && hour <= 4;
            minutes = (hour + (dayPart == 0 || nextMorning ? 0 : 12)) * 60 + minutes % 60;
        }
        if (dayPart == 3 && minutes < 5 * 60)
            date = date.addDays(1);
    }
    // Tonight without a time is eight o'clock or, once eight has gone by, the
    // next hour on the clock: after eleven that is midnight, moved on below.
    if (tonight && minutes < 0)
        minutes = date == today ? qMax(20 * 60, (nowMinutes / 60 + 1) * 60) : 20 * 60;
    // Tonight runs from the evening into the small hours. Without am or pm,
    // "tonight at 9" is 21:00 and "tonight at 12" midnight, and a time before
    // noon that has already gone by belongs to the night ahead.
    if (tonight && date == today) {
        // "5:30" comes in as 17:30 on a 12-hour clock and as 5:30 on a 24-hour
        // one, so the hour is read on the 12-hour clock: 5 to 11 are the
        // evening, 12 midnight, and 1 to 4 the small hours.
        if (guessed) {
            const int hour = minutes / 60 % 12;
            minutes = (hour >= 5 ? hour + 12 : hour) * 60 + minutes % 60;
        }
        if (minutes >= 24 * 60)
            minutes -= 24 * 60;
        if (minutes < 12 * 60 && minutes <= nowMinutes)
            date = date.addDays(1);
    }
    // "Every friday at 9" starts today on a Friday, unless nine has gone by.
    if (repeatDay) {
        const bool implicit = !date.isValid();
        date = Repeat::onWeekday(date.isValid() ? date : today, repeatDay);
        if (implicit && date == today && minutes >= 0 && Dates::nextClockDate(minutes, now, lateFold) != today)
            date = date.addDays(7);
    }
    if (minutes >= 0 && !date.isValid()) {
        // A time on its own is the next time the clock shows it.
        date = Dates::nextClockDate(minutes, now, lateFold);
    }
    if (repeat.repeats() && !date.isValid())
        date = today;
    if (repeat.unit == Repeat::Weekdays) {
        const QDate workday = Repeat::firstWorkday(date);
        if (workday != date)
            lateFold = false;
        date = workday;
    }
    if (repeat.unit == Repeat::Monthly || repeat.unit == Repeat::Yearly)
        repeat.anchorDay = date.day();

    Parsed parsed;
    QStringList title;
    for (int i = 0; i < size; ++i)
        if (!used[i])
            title << tokens[i];
    parsed.leftover = title.size();
    parsed.entry.title = title.join(' ');
    parsed.entry.due = date;
    parsed.entry.minutes = date.isValid() ? minutes : -1;
    parsed.entry.repeat = repeat.toString();
    parsed.entry.tags = tags;
    parsed.entry.priority = priority;
    parsed.entry.repeatEnd = repeatEnd;
    parsed.entry.weekday = repeatDay;
    parsed.entry.lateFold = lateFold;
    return parsed;
}

QString translate(const char *text) { return QCoreApplication::translate("Dates", text); }

} // namespace

QDate Dates::nextClockDate(int minutes, const QDateTime &now, bool &lateFold) {
    const QTime time(minutes / 60, minutes % 60);
    const QTimeZone zone = now.timeRepresentation();
    lateFold = false;
    if (!now.isValid() || !time.isValid())
        return {};
    for (QDate day = now.date(); day.isValid() && day.year() <= 9999; day = day.addDays(1)) {
        const QDateTime first(day, time, zone, QDateTime::TransitionResolution::PreferBefore);
        if (first.date() != day || first.time() != time)
            continue; // the clock skipped this minute
        if (first > now)
            return day;
        const QDateTime later(day, time, zone, QDateTime::TransitionResolution::PreferAfter);
        if (later > now) {
            lateFold = true;
            return day;
        }
    }
    return {};
}

QuickEntry Dates::parseQuick(const QString &text, const QDateTime &now) {
    const Parsed parsed = parse(text, now, false);
    // A line that is nothing but a date is a title, not a reminder without one.
    if (parsed.entry.title.isEmpty() || (parsed.entry.repeatEnd.isValid() && parsed.entry.repeatEnd < parsed.entry.due)) {
        QuickEntry plain;
        plain.title = text.simplified();
        return plain;
    }
    return parsed.entry;
}

QDate Dates::parseDate(const QString &text, const QDate &today, const QLocale &locale) {
    const QString phrase = text.simplified();
    if (phrase.isEmpty())
        return {};
    if (locale == QLocale()) {
        if (phrase.compare(translate(QT_TRANSLATE_NOOP("Dates", "Today")), Qt::CaseInsensitive) == 0)
            return today;
        if (phrase.compare(translate(QT_TRANSLATE_NOOP("Dates", "Tomorrow")), Qt::CaseInsensitive) == 0)
            return today.addDays(1);
        if (phrase.compare(translate(QT_TRANSLATE_NOOP("Dates", "Yesterday")), Qt::CaseInsensitive) == 0)
            return today.addDays(-1);
    }
    if (phrase.compare("yesterday", Qt::CaseInsensitive) == 0)
        return today.addDays(-1);
    if (const QDate exact = locale.toDate(phrase, editablePattern(locale), 1970); exact.isValid())
        return exact;
    if (const QDate exact = locale.toDate(phrase, dayPattern(locale, true), 1970); exact.isValid())
        return exact;
    const QString thisYear = phrase + ' ' + locale.toString(today, "yyyy");
    if (const QDate exact = locale.toDate(thisYear, dayPattern(locale, false) + " yyyy", 1970); exact.isValid())
        return exact;
    for (int day = 1; day <= 7; ++day) {
        if (phrase.compare(locale.dayName(day), Qt::CaseInsensitive) == 0) {
            const QDate next = Repeat::onWeekday(today, day);
            return next == today ? next.addDays(7) : next;
        }
    }
    const Parsed parsed = parse(phrase, QDateTime(today, QTime(0, 0)), true, locale);
    if (parsed.entry.due.isValid() && parsed.leftover == 0 && parsed.entry.minutes < 0)
        return parsed.entry.due;

    // Just a number is that day of this month, or of the next once it has passed.
    if (const int day = dayNumber(phrase.toLower())) {
        QDate found(today.year(), today.month(), qMin(day, today.daysInMonth()));
        if (found < today) {
            const QDate following = QDate(today.year(), today.month(), 1).addMonths(1);
            found = QDate(following.year(), following.month(), qMin(day, following.daysInMonth()));
        }
        return found;
    }

    for (const auto format : {QLocale::ShortFormat, QLocale::LongFormat}) {
        const QDate found = locale.toDate(phrase, format, 1970);
        if (found.isValid())
            return found;
    }
    return numericDate(phrase, today, locale);
}

int Dates::parseTime(const QString &text, const QLocale &locale) {
    const QString phrase = text.simplified().toLower();
    if (phrase.isEmpty())
        return -1;
    QStringList words = phrase.split(' ');
    if (words.first() == "at")
        words.removeFirst();
    const Time time = timeAt(words, 0, locale);
    if (time.minutes >= 0 && time.used == words.size())
        return time.minutes;

    static const QRegularExpression digits("^(\\d{2})(\\d{2})$");
    const auto match = digits.match(phrase);
    if (match.hasMatch() && match.captured(1).toInt() < 24 && match.captured(2).toInt() < 60)
        return match.captured(1).toInt() * 60 + match.captured(2).toInt();

    const QTime local = locale.toTime(text.simplified(), QLocale::ShortFormat);
    return local.isValid() ? local.hour() * 60 + local.minute() : -1;
}

// The locale's long date without the parts a list of reminders can do
// without: "Friday, November 20, 2026" becomes "Fri, Nov 20", and
// "fredag den 20. november 2026" becomes "fre. den 20. nov.".
QString Dates::dayPattern(const QLocale &locale, bool withYear) {
    QString pattern = locale.dateFormat(QLocale::LongFormat);
    pattern.replace("dddd", "ddd");
    pattern.replace("MMMM", "MMM");
    if (!withYear) {
        static const QRegularExpression yearPart(
            R"((\s*'[^']*')?[\s,./-]*y+\s*$|^y+\s*[^\sdM']?[\s,./-]*|[\s,./-]*y+)");
        pattern.remove(yearPart);
    }
    return pattern.trimmed();
}

QString Dates::formatDay(const QDate &date, const QDate &today) {
    if (!date.isValid())
        return {};
    const qint64 days = today.daysTo(date);
    if (days == 0)
        return translate(QT_TRANSLATE_NOOP("Dates", "Today"));
    if (days == 1)
        return translate(QT_TRANSLATE_NOOP("Dates", "Tomorrow"));
    if (days == -1)
        return translate(QT_TRANSLATE_NOOP("Dates", "Yesterday"));
    const QLocale locale;
    if (days > 1 && days < 7)
        return locale.toString(date, "dddd");
    return locale.toString(date, dayPattern(locale, date.year() != today.year()));
}

QString Dates::formatEditableDate(const QDate &date) {
    const QLocale locale;
    return date.isValid() ? locale.toString(date, editablePattern(locale)) : QString();
}
QString Dates::formatTime(int minutes) {
    if (minutes < 0)
        return {};
    return QLocale().toString(QTime(minutes / 60, minutes % 60), QLocale::ShortFormat);
}

QString Dates::formatDue(const QDate &date, int minutes, const QDate &today) {
    const QString day = formatDay(date, today);
    return minutes >= 0 && !day.isEmpty() ? day + ", " + formatTime(minutes) : day;
}

QString Dates::formatMonth(const QDate &date) {
    return QLocale().toString(date, "MMMM yyyy");
}
