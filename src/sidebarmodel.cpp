#include "sidebarmodel.h"

int SidebarModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QHash<int, QByteArray> SidebarModel::roleNames() const {
    return {{KeyRole, "key"},     {GroupRole, "group"}, {NameRole, "name"},
            {ColorRole, "tint"}, {IconRole, "icon"},   {CountRole, "count"}};
}

QVariant SidebarModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const SidebarEntry &entry = m_entries[index.row()];
    switch (role) {
    case KeyRole: return entry.key;
    case GroupRole: return entry.group;
    case NameRole: return entry.name;
    case ColorRole: return entry.color;
    case IconRole: return entry.icon;
    case CountRole: return entry.count;
    }
    return {};
}

void SidebarModel::show(const QVector<SidebarEntry> &entries) {
    if (entries == m_entries)
        return;
    bool sameKeys = entries.size() == m_entries.size();
    for (int i = 0; sameKeys && i < entries.size(); ++i)
        sameKeys = entries[i].key == m_entries[i].key;
    // Counts change with every edit; the entries themselves rarely do.
    if (sameKeys && !entries.isEmpty()) {
        m_entries = entries;
        emit dataChanged(index(0), index(m_entries.size() - 1));
        return;
    }
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

int SidebarModel::indexOf(const QString &key) const {
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].key == key)
            return i;
    return -1;
}
