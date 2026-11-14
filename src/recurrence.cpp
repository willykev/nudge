#include "recurrence.h"

#include <QStringList>

Repeat Repeat::parse(const QString &rule) {
    Repeat repeat;
    const QStringList parts = rule.split(':');
    const QString name = parts.value(0);
    repeat.unit = name == "daily"      ? Daily
                  : name == "weekdays" ? Weekdays
                  : name == "weekly"   ? Weekly
                  : name == "monthly"  ? Monthly
                  : name == "yearly"   ? Yearly
                                       : Never;
    repeat.interval = qBound(1, parts.value(1, "1").toInt(), 999);
    repeat.anchorDay = qBound(0, parts.value(2, "0").toInt(), 31);
    if (repeat.unit == Never || repeat.unit == Weekdays)
        repeat.interval = 1;
    return repeat;
}

QString Repeat::toString() const {
    const QString count = QString::number(qMax(1, interval));
    const QString anchor = anchorDay > 0 ? ":" + QString::number(anchorDay) : QString();
    switch (unit) {
    case Daily: return "daily:" + count;
    case Weekdays: return "weekdays";
    case Weekly: return "weekly:" + count;
    case Monthly: return "monthly:" + count + anchor;
    case Yearly: return "yearly:" + count + anchor;
    case Never: break;
    }
    return {};
}

QDate Repeat::after(const QDate &date) const {
    if (!date.isValid())
        return {};
    const int step = qMax(1, interval);
    const int day = anchorDay > 0 ? anchorDay : date.day();
    switch (unit) {
    case Daily:
        return date.addDays(step);
    case Weekdays: {
        QDate next = date.addDays(1);
        while (next.dayOfWeek() > 5)
            next = next.addDays(1);
        return next;
    }
    case Weekly:
        return date.addDays(7LL * step);
    case Monthly: {
        const QDate month = QDate(date.year(), date.month(), 1).addMonths(step);
        return QDate(month.year(), month.month(), qMin(day, month.daysInMonth()));
    }
    case Yearly: {
        const QDate month(date.year() + step, date.month(), 1);
        return QDate(month.year(), month.month(), qMin(day, month.daysInMonth()));
    }
    case Never:
        break;
    }
    return {};
}

QDate Repeat::upcoming(const QDate &due, const QDate &today, const QDate &end) const {
    QDate next = after(due);
    // A reminder that sat overdue for a month should come back next, not
    // thirty times in a row. The cap only guards against a broken rule.
    for (int i = 0; next.isValid() && next < today && i < 100000; ++i)
        next = after(next);
    if (!next.isValid() || (end.isValid() && next > end))
        return {};
    return next;
}
