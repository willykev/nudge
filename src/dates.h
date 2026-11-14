#pragma once

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QStringList>

// What a line of typed text says about a reminder besides its title:
// "Call the dentist tomorrow at 9 #health !!" is a title, a date, a time,
// a tag, and a priority.
struct QuickEntry {
    QString title;
    QDate due;
    int minutes = -1;
    QString repeat;
    QStringList tags;
    int priority = 0;
};

namespace Dates {
// The words are English. Other languages type dates the way their locale
// writes them, or pick one from the calendar.
QuickEntry parseQuick(const QString &text, const QDateTime &now);
QDate parseDate(const QString &text, const QDate &today); // invalid when not understood
int parseTime(const QString &text);                       // minutes after midnight, or -1

QString formatDay(const QDate &date, const QDate &today); // Today, Friday, Fri, Nov 20
QString formatTime(int minutes);
QString formatDue(const QDate &date, int minutes, const QDate &today);
QString formatMonth(const QDate &date);
QString dayPattern(const QLocale &locale, bool withYear);
} // namespace Dates
