#include "include/ui/utils/ProfilesFilterProxyModel.h"
#include "include/ui/utils/ProfilesTableModel.h"

namespace {
    const QString portPrefix = QStringLiteral("port=");
}

ProfilesFilterProxyModel::ProfilesFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent) {
    // Re-tests a row on dataChanged, so test results arriving mid-run move it in
    // and out of an active filter on their own.
    setDynamicSortFilter(true);
}

ProfilesTableModel *ProfilesFilterProxyModel::profilesModel() const {
    return qobject_cast<ProfilesTableModel *>(sourceModel());
}

bool ProfilesFilterProxyModel::hasActiveFilter() const {
    return !m_type.isEmpty() || !m_address.isEmpty() || !m_name.isEmpty() || !m_country.isEmpty();
}

void ProfilesFilterProxyModel::setFilters(const QString &type, const QString &address,
                                          const QString &name, const QString &country) {
    if (m_type == type && m_address == address && m_name == name && m_country == country) return;
    m_type = type;
    m_address = address;
    m_name = name;
    m_country = country;
    invalidateRowsFilter();
}

int ProfilesFilterProxyModel::toSourceRow(int proxyRow) const {
    const QModelIndex idx = mapToSource(index(proxyRow, 0));
    return idx.isValid() ? idx.row() : -1;
}

int ProfilesFilterProxyModel::toProxyRow(int sourceRow) const {
    if (!sourceModel()) return -1;
    const QModelIndex idx = mapFromSource(sourceModel()->index(sourceRow, 0));
    return idx.isValid() ? idx.row() : -1;
}

bool ProfilesFilterProxyModel::portMatches(int port) const {
    const QString val = m_address.mid(portPrefix.size());
    if (!val.contains(':')) return val.isEmpty() ? false : port == val.toInt();
    const QStringList parts = val.split(':');
    const bool minOk = parts[0].isEmpty() || port >= parts[0].toInt();
    const bool maxOk = (parts.size() < 2 || parts[1].isEmpty()) || port <= parts[1].toInt();
    return minOk && maxOk;
}

bool ProfilesFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &) const {
    auto *model = profilesModel();
    if (!model) return true;

    if (model->isGroupHeader(sourceRow)) {
        return !hasActiveFilter() || groupHasMatchingProfile(sourceRow);
    }
    return !hasActiveFilter() || profileMatches(sourceRow);
}

bool ProfilesFilterProxyModel::groupHasMatchingProfile(int headerRow) const {
    auto *model = profilesModel();
    if (!model) return false;
    for (int row = headerRow + 1; row < model->rowCount(); ++row) {
        if (model->isGroupHeader(row)) break;
        if (profileMatches(row)) return true;
    }
    return false;
}

bool ProfilesFilterProxyModel::profileMatches(int sourceRow) const {
    auto *model = profilesModel();
    if (!model || model->isGroupHeader(sourceRow)) return false;

    // A profile that failed to load stays visible rather than becoming unreachable.
    const auto *key = model->filterKeyAt(sourceRow);
    if (!key) return true;

    if (!m_address.isEmpty()) {
        if (m_address.startsWith(portPrefix)) {
            if (!portMatches(key->port)) return false;
        } else if (!key->address.contains(m_address, Qt::CaseInsensitive)) {
            return false;
        }
    }
    if (!m_name.isEmpty() && !key->name.contains(m_name, Qt::CaseInsensitive)) return false;
    if (!m_type.isEmpty() && !key->type.contains(m_type, Qt::CaseInsensitive)) return false;
    if (!m_country.isEmpty() && !key->country.contains(m_country, Qt::CaseInsensitive)) return false;
    return true;
}
