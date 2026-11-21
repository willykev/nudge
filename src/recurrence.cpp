#include "recurrence.h"

#include <QStringList>
#include <QTimeZone>

namespace {
QDate representable(const QDate &date) {
    return date.isValid() && date.year() >= 1 && date.year() <= 9999 ? date : QDate();
}
}

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

QString Repeat::anchored(const QDate &date, const QString &previousRule) const {
    Repeat rule = *this;
    if (unit == Monthly || unit == Yearly) {
        const Repeat previous = parse(previousRule);
        rule.anchorDay = previous.unit == unit && previous.interval == interval && previous.anchorDay
                             ? previous.anchorDay : date.day();
    }
    return rule.toString();
}

QDate Repeat::onWeekday(const QDate &date, int day) {
    return representable(date.addDays((day - date.dayOfWeek() + 7) % 7));
}

QDate Repeat::firstWorkday(const QDate &date) {
    return date.dayOfWeek() > 5 ? onWeekday(date, 1) : representable(date);
}

QDate Repeat::after(const QDate &date) const {
    if (!representable(date).isValid())
        return QDate();
    const int step = qMax(1, interval);
    const int day = anchorDay > 0 ? anchorDay : date.day();
    switch (unit) {
    case Daily:
        return representable(date.addDays(step));
    case Weekdays:
        return firstWorkday(date.addDays(1));
    case Weekly:
        return representable(date.addDays(7LL * step));
    case Monthly: {
        const QDate month = QDate(date.year(), date.month(), 1).addMonths(step);
        return representable(QDate(month.year(), month.month(), qMin(day, month.daysInMonth())));
    }
    case Yearly: {
        const QDate month(date.year() + step, date.month(), 1);
        return representable(QDate(month.year(), month.month(), qMin(day, month.daysInMonth())));
    }
    case Never:
        break;
    }
    return {};
}

QDate Repeat::upcoming(const QDate &due, const QDate &today, const QDate &end) const {
    QDate next = after(due);
    if (next.isValid() && next < today) {
        const int step = qMax(1, interval);
        if (unit == Weekdays) {
            next = firstWorkday(today);
        } else if (unit == Daily || unit == Weekly) {
            const qint64 stride = unit == Daily ? step : 7LL * step;
            const qint64 distance = due.daysTo(today);
            next = representable(due.addDays(((distance + stride - 1) / stride) * stride));
        } else {
            const int stride = unit == Monthly ? step : 12 * step;
            const int months = (today.year() - due.year()) * 12 + today.month() - due.month();
            const int day = anchorDay ? anchorDay : due.day();
            QDate month = QDate(due.year(), due.month(), 1).addMonths((months / stride) * stride);
            next = QDate(month.year(), month.month(), qMin(day, month.daysInMonth()));
            if (next < today) {
                month = month.addMonths(stride);
                next = QDate(month.year(), month.month(), qMin(day, month.daysInMonth()));
            }
            next = representable(next);
        }
    }
    if (!next.isValid() || (end.isValid() && next > end))
        return {};
    return next;
}

QDate Repeat::upcoming(const QDate &due, int minutes, const QDateTime &now, const QDate &end) const {
    const bool gone =
        minutes >= 0 && QDateTime(now.date(), QTime(minutes / 60, minutes % 60), now.timeRepresentation()) <= now;
    return upcoming(due, gone ? now.date().addDays(1) : now.date(), end);
}
