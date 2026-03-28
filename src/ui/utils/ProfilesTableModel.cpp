#include "include/ui/utils/ProfilesTableModel.h"
#include "include/global/Configs.hpp"
#include "include/database/entities/Profile.h"
#include "include/configs/common/Outbound.h"
#include <QApplication>
#include <QMimeData>
#include <QPalette>

#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"

ProfilesTableModel::ProfilesTableModel(QObject *parent)
    : QAbstractTableModel(parent) {}

int ProfilesTableModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return m_profileIds.size();
}

int ProfilesTableModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return 11;
}

Qt::ItemFlags ProfilesTableModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags defaultFlags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        const int profileId = m_profileIds.value(index.row(), -1);
        const bool disabled = Configs::dataManager->settingsRepo->disabled_profile_ids.contains(QString::number(profileId));

        if (index.column() == 1) {
            if (disabled) return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
            return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        }
        if (index.column() == 0) {
            if (disabled) return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
            return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        }
        if (disabled) {
            return Qt::ItemIsEnabled;
        }
        return Qt::ItemIsDragEnabled | defaultFlags;
    }
    return Qt::ItemIsDropEnabled | defaultFlags;
}

Qt::DropActions ProfilesTableModel::supportedDropActions() const {
    return Qt::MoveAction;
}

QStringList ProfilesTableModel::mimeTypes() const {
    return {"application/profile-row-number"};
}

QMimeData* ProfilesTableModel::mimeData(const QModelIndexList &indexes) const {
    auto *mimeData = new QMimeData;
    QByteArray encodedData;

    QDataStream stream(&encodedData, QIODevice::WriteOnly);

    if (!indexes.isEmpty()) {
        stream << indexes.at(0).row();
    }

    mimeData->setData("application/profile-row-number", encodedData);
    return mimeData;
}

void ProfilesTableModel::ensureCached(int profileId) const {
    if (m_cache.contains(profileId)) {
        for (int i = 0; i < m_lruOrder.size(); ++i) {
            if (m_lruOrder[i] == profileId) {
                m_lruOrder.move(i, m_lruOrder.size() - 1);
                break;
            }
        }
        return;
    }

    auto profile = Configs::dataManager->profilesRepo->GetProfile(profileId);
    if (!profile) return;

    while (m_cache.size() >= m_cacheSize && !m_lruOrder.isEmpty()) {
        evictOne();
    }
    m_cache[profileId] = profile;
    m_lruOrder.append(profileId);
}

void ProfilesTableModel::evictOne() const {
    if (m_lruOrder.isEmpty()) return;
    int id = m_lruOrder.takeFirst();
    m_cache.remove(id);
}

QVariant ProfilesTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_profileIds.size()
        || index.column() < 0 || index.column() >= 11) {
        return {};
    }
    const int profileId = m_profileIds[index.row()];
    if (role == ProfileIdRole) {
        return profileId;
    }
    ensureCached(profileId);
    auto it = m_cache.constFind(profileId);
    if (it == m_cache.constEnd()) return {};
    const std::shared_ptr<Configs::Profile> &profile = it.value();
    if (!profile) return {};

    const int startedId = Configs::dataManager->settingsRepo->started_id;
    const bool isRunning = (profile->id == startedId);
    QColor linkColor = isRunning ? QApplication::palette().link().color() : QColor();

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return QString();
        case 1: return QString();
        case 2: return profile->outbound ? profile->outbound->DisplayType() : QString();
        case 3: return profile->outbound ? profile->outbound->DisplayAddress() : QString();
        case 4: return profile->outbound ? profile->outbound->name : QString();
        case 5: return profile->DisplayLatency();
        case 6: return profile->DisplayRxSpeed();
        case 7: return profile->DisplayConnectionTime();
        case 8: return profile->DisplaySiteScore();
        case 9: return profile->DisplayTrafficRx();
        case 10: return profile->DisplayTrafficTx();
        default: return {};
        }
    }
    if (role == Qt::CheckStateRole && index.column() == 0) {
        const auto startupIds = Configs::dataManager->settingsRepo->speedtest_on_startup_profile_ids;
        return startupIds.contains(QString::number(profileId)) ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::CheckStateRole && index.column() == 1) {
        const auto disabledIds = Configs::dataManager->settingsRepo->disabled_profile_ids;
        return disabledIds.contains(QString::number(profileId)) ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::TextAlignmentRole && (index.column() == 0 || index.column() == 1)) {
        return static_cast<int>(Qt::AlignCenter);
    }
    const auto disabledIds = Configs::dataManager->settingsRepo->disabled_profile_ids;
    const bool isDisabledRow = disabledIds.contains(QString::number(profileId));

    if (role == Qt::BackgroundRole && isDisabledRow) {
        return QColor(25, 25, 25);
    }
    if (role == Qt::ForegroundRole) {
        if (isDisabledRow) {
            return QColor(Qt::darkGray);
        }
        if (index.column() == 5) {
            QColor latencyColor = profile->DisplayLatencyColor();
            if (latencyColor.isValid()) return latencyColor;
        }
        if (index.column() == 6) {
            const auto rxText = profile->DisplayRxSpeed().trimmed();
            if (rxText.compare("N/A", Qt::CaseInsensitive) == 0 ||
                rxText.compare("Unavailable", Qt::CaseInsensitive) == 0) {
                return QColor(Qt::darkGray);
            }
        }
        if (index.column() == 7) {
            if (profile->connect_time_ms < 0) {
                return QColor(Qt::darkGray);
            }
            if (profile->connect_time_ms > 0) {
                if (profile->connect_time_ms <= 100) return QColor(Qt::darkGreen);
                if (profile->connect_time_ms <= 300) return QColor(Qt::darkYellow);
                return QColor(Qt::red);
            }
        }
        if (index.column() == 8 && profile->site_score > 0) {
            if (profile->site_score >= 80) return QColor(Qt::darkGreen);
            if (profile->site_score >= 55) return QColor(Qt::darkYellow);
            return QColor(Qt::red);
        }
        if (isRunning && linkColor.isValid()) return linkColor;
        return {};
    }
    return {};
}

bool ProfilesTableModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() || role != Qt::CheckStateRole) return false;
    const int profileId = m_profileIds[index.row()];
    const QString idStr = QString::number(profileId);
    const bool checked = (value.toInt() == Qt::Checked);

    if (index.column() == 0) {
        auto startupIds = Configs::dataManager->settingsRepo->speedtest_on_startup_profile_ids;
        if (checked) {
            if (!startupIds.contains(idStr)) startupIds.append(idStr);
        } else {
            startupIds.removeAll(idStr);
        }
        Configs::dataManager->settingsRepo->speedtest_on_startup_profile_ids = startupIds;
    } else if (index.column() == 1) {
        auto disabledIds = Configs::dataManager->settingsRepo->disabled_profile_ids;
        if (checked) {
            if (!disabledIds.contains(idStr)) disabledIds.append(idStr);
        } else {
            disabledIds.removeAll(idStr);
        }
        Configs::dataManager->settingsRepo->disabled_profile_ids = disabledIds;
    } else {
        return false;
    }

    Configs::dataManager->settingsRepo->Save();
    emit dataChanged(this->index(index.row(), index.column()), this->index(index.row(), columnCount() - 1),
                     {Qt::CheckStateRole, Qt::ForegroundRole, Qt::BackgroundRole});
    return true;
}

QVariant ProfilesTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case 0: return tr("Speedtest on startup");
        case 1: return tr("Disabled");
        case 2: return tr("Type");
        case 3: return tr("Address");
        case 4: return tr("Name");
        case 5: return tr("Latency");
        case 6: return tr("Rx speed");
        case 7: return tr("Connection time");
        case 8: return tr("Site Score");
        case 9: return tr("Rx");
        case 10: return tr("Tx");
        default: return {};
        }
    }
    return {};
}

void ProfilesTableModel::setProfileIds(const QList<int> &ids) {
    beginResetModel();
    m_profileIds = ids;
    id2row.clear();
    int idx=0;
    for (const auto &id : ids) {
        id2row.insert(id, idx++);
    }
    m_cache.clear();
    m_lruOrder.clear();
    endResetModel();
}

void ProfilesTableModel::refreshTable(const QList<int> &ids, bool mayNeedReset) {
    if (m_profileIds.isEmpty() && ids.isEmpty()) return;

    bool needFullReset = (ids.length() != m_profileIds.length()) && mayNeedReset;
    if (!needFullReset && !ids.isEmpty() && mayNeedReset) {
        for (int i=0; i < ids.length(); i++) {
            if (ids[i] != m_profileIds[i]) {
                needFullReset = true;
                break;
            }
        }
    }

    if (needFullReset) {
        setProfileIds(ids);
    } else {
        // Soft refresh path: clear cache to avoid showing stale profile snapshots.
        m_cache.clear();
        m_lruOrder.clear();
        QModelIndex topLeft = index(0, 0);
        QModelIndex bottomRight = index(m_profileIds.count() - 1, columnCount() - 1);

        emit dataChanged(topLeft, bottomRight);
    }
}

int ProfilesTableModel::profileIdAt(int row) const {
    if (row < 0 || row >= m_profileIds.size()) return -1;
    return m_profileIds[row];
}

int ProfilesTableModel::rowOfProfileId(int profileId) const {
    return id2row.value(profileId, -1);
}

bool ProfilesTableModel::moveProfileRow(int fromRow, int toRow) {
    if (fromRow < 0 || toRow < 0 || fromRow >= m_profileIds.size() || toRow >= m_profileIds.size()) return false;
    if (fromRow == toRow) return true;

    const int destinationChild = (fromRow < toRow) ? (toRow + 1) : toRow;
    if (!beginMoveRows(QModelIndex(), fromRow, fromRow, QModelIndex(), destinationChild)) return false;

    const int movedId = m_profileIds.takeAt(fromRow);
    m_profileIds.insert(toRow, movedId);

    id2row.clear();
    for (int i = 0; i < m_profileIds.size(); ++i) {
        id2row.insert(m_profileIds[i], i);
    }

    endMoveRows();
    return true;
}

void ProfilesTableModel::refreshProfileId(int profileId) {
    if (!id2row.contains(profileId)) return;
    m_cache.remove(profileId);
    for (int i = 0; i < m_lruOrder.size(); ++i) {
        if (m_lruOrder[i] == profileId) {
            m_lruOrder.removeAt(i);
            break;
        }
    }
    auto r = id2row.value(profileId);
    QModelIndex top = index(r, 0);
    QModelIndex bottom = index(r, columnCount() - 1);
    emit dataChanged(top, bottom);
}

void ProfilesTableModel::emplaceProfiles(int row1, int row2) {
    if (m_profileIds.size() <= row1 || m_profileIds.size() <= row2) return;
    m_profileIds.insert(row2+1, m_profileIds[row1]);
    if (row1 < row2) m_profileIds.remove(row1);
    else m_profileIds.remove(row1+1);
    id2row.clear();
    for (int i = 0; i < m_profileIds.size(); ++i) {
        id2row.insert(m_profileIds[i], i);
    }
    for (int i = std::max(std::min(row1, row2), 0); i <= std::max(row1, row2); ++i) {
        refreshProfileId(m_profileIds[i]);
    }
}

QString ProfilesTableModel::rowLabel(int row) const {
    if (row < 0 || row >= m_profileIds.size()) return {};
    int id = m_profileIds[row];
    if (Configs::dataManager->settingsRepo->started_id == id) {
        return QStringLiteral("✓");
    }
    return QString::number(row + 1) + QStringLiteral("  ");
}
