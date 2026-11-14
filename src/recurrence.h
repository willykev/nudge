#pragma once

#include <QDate>
#include <QString>

// How a reminder repeats, as stored on it: "" (never), "daily:N", "weekdays",
// "weekly:N", "monthly:N:D" and "yearly:N:D". D is the day of the month the
// series keeps returning to, so a reminder on the 31st lands on the 28th in
// February and is back on the 31st in March.
struct Repeat {
    enum Unit { Never, Daily, Weekdays, Weekly, Monthly, Yearly };

    Unit unit = Never;
    int interval = 1;
    int anchorDay = 0;

    static Repeat parse(const QString &rule);
    QString toString() const;
    bool repeats() const { return unit != Never; }

    // The occurrence that follows `date`.
    QDate after(const QDate &date) const;

    // The occurrence after `due` that is not already in the past, or an
    // invalid date once the series has run beyond `end`.
    QDate upcoming(const QDate &due, const QDate &today, const QDate &end = {}) const;
};
