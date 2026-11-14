#pragma once

#include <QAbstractListModel>
#include <QVector>

struct SidebarEntry {
    QString key;   // the view it opens: "today", "list:3", "tag:home"
    QString group; // "", "lists", or "tags"
    QString name;
    QString color; // empty for the theme accent
    QString icon;
    int count = -1; // -1 shows no count

    bool operator==(const SidebarEntry &) const = default;
};

// The views down the left of the window: the built-in ones, the user's
// lists, and the tags in use.
class SidebarModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role { KeyRole = Qt::UserRole + 1, GroupRole, NameRole, ColorRole, IconRole, CountRole };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void show(const QVector<SidebarEntry> &entries);
    const QVector<SidebarEntry> &entries() const { return m_entries; }
    int indexOf(const QString &key) const;

private:
    QVector<SidebarEntry> m_entries;
};
