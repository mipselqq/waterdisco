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
    return ColumnCount;
}

Qt::ItemFlags ProfilesTableModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags defaultFlags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        const int profileId = m_profileIds.value(index.row(), -1);
        const bool disabled = profileId >= 0
            && Configs::dataManager->settingsRepo->IsProfileDisabled(profileId);
        if (index.column() == ColStartup || index.column() == ColDisabled) {
            Qt::ItemFlags flags = Qt::ItemIsEnabled;
            if (!disabled || index.column() == ColDisabled) {
                flags |= Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
            }
            if (!disabled) flags |= Qt::ItemIsDragEnabled;
            return flags;
        }
        if (disabled) return Qt::ItemIsEnabled;
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
        || index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }
    const int profileId = m_profileIds[index.row()];
    if (role == ProfileIdRole) {
        return profileId;
    }
    if (role == Qt::CheckStateRole) {
        if (index.column() == ColStartup) {
            return Configs::dataManager->settingsRepo->IsStartupProfile(profileId)
                ? Qt::Checked : Qt::Unchecked;
        }
        if (index.column() == ColDisabled) {
            return Configs::dataManager->settingsRepo->IsProfileDisabled(profileId)
                ? Qt::Checked : Qt::Unchecked;
        }
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
        case ColStartup:
        case ColDisabled: return QString();
        case ColType: {
            if (!profile->outbound) return QString();
            auto type = profile->outbound->DisplayType();
            if (Configs::dataManager->settingsRepo->show_config_security) {
                auto sec = profile->outbound->DisplaySecurity();
                if (!sec.isEmpty()) type += QStringLiteral(" (%1)").arg(sec);
            }
            return type;
        }
        case ColAddress: return profile->outbound ? profile->outbound->DisplayAddress() : QString();
        case ColName: return profile->outbound ? profile->outbound->name : QString();
        case ColLatency: return profile->DisplayLatency();
        case ColRxSpeed: return profile->DisplayRxSpeed();
        case ColConnectionTime: return profile->DisplayConnectionTime();
        case ColSiteScore: return profile->DisplaySiteScore();
        case ColRxTraffic: return profile->DisplayTrafficRx();
        case ColTxTraffic: return profile->DisplayTrafficTx();
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColStartup:
        case ColDisabled:
        case ColLatency:
        case ColRxSpeed:
        case ColConnectionTime:
        case ColSiteScore:
        case ColRxTraffic:
        case ColTxTraffic:
            return static_cast<int>(Qt::AlignCenter);
        default:
            return static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);
        }
    }
    if (role == Qt::ToolTipRole) {
        if (index.column() == ColType && Configs::dataManager->settingsRepo->show_config_security
            && profile->outbound && profile->outbound->GetSecurity().isDangerous()) {
            return tr("This config's traffic is not properly protected.");
        }
        return {};
    }
    if (role == Qt::ForegroundRole) {
        if (Configs::dataManager->settingsRepo->IsProfileDisabled(profileId)) {
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        }
        if (index.column() == ColLatency) {
            QColor latencyColor = profile->DisplayLatencyColor();
            if (latencyColor.isValid()) return latencyColor;
        }
        if (index.column() == ColRxSpeed) {
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Error) {
                return QColor(Qt::red);
            }
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Skipped) {
                return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
            }
        }
        if (index.column() == ColConnectionTime && profile->connect_time_ms > 0) {
            if (profile->connect_time_ms <= 100) return QColor(Qt::darkGreen);
            if (profile->connect_time_ms <= 300) return QColor(Qt::darkYellow);
            return QColor(Qt::red);
        }
        if (index.column() == ColSiteScore
            && profile->performance_test_status == Configs::PerformanceTestStatus::Success) {
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
    if (!index.isValid() || role != Qt::CheckStateRole
        || index.row() < 0 || index.row() >= m_profileIds.size()) {
        return false;
    }

    const int profileId = m_profileIds[index.row()];
    const bool checked = value.toInt() == Qt::Checked;
    auto *settings = Configs::dataManager->settingsRepo.get();
    if (index.column() == ColStartup) {
        settings->SetStartupProfile(profileId, checked);
    } else if (index.column() == ColDisabled) {
        settings->SetProfileDisabled(profileId, checked);
    } else {
        return false;
    }
    settings->Save();
    const int lastChangedColumn = index.column() == ColDisabled
        ? ColumnCount - 1 : ColStartup;
    emit dataChanged(this->index(index.row(), ColStartup),
                     this->index(index.row(), lastChangedColumn),
                     {Qt::DisplayRole, Qt::CheckStateRole, Qt::ForegroundRole});
    return true;
}

QVariant ProfilesTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignCenter);
    }
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case ColStartup: return tr("Speedtest\non startup");
        case ColDisabled: return tr("Off");
        case ColType: return tr("Type");
        case ColAddress: return tr("Address");
        case ColName: return tr("Name");
        case ColLatency: return tr("Latency");
        case ColRxSpeed: return tr("Rx speed");
        case ColConnectionTime: return tr("Connection\ntime");
        case ColSiteScore: return tr("Site Score");
        case ColRxTraffic: return tr("Rx traffic");
        case ColTxTraffic: return tr("Tx traffic");
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
    m_filterKeys.clear();
    m_filterIndexBuilt = false;
    endResetModel();
}

namespace {
    ProfilesTableModel::FilterKey makeFilterKey(const std::shared_ptr<Configs::Profile> &profile) {
        ProfilesTableModel::FilterKey key;
        key.type = profile->type;
        key.country = profile->test_country;
        if (profile->outbound) {
            key.address = profile->outbound->server;
            key.name = profile->outbound->name;
            key.port = profile->outbound->server_port;
        }
        return key;
    }
}

void ProfilesTableModel::ensureFilterIndex() const {
    if (m_filterIndexBuilt) return;
    m_filterIndexBuilt = true;
    m_filterKeys.clear();
    m_filterKeys.reserve(m_profileIds.size());
    for (const auto &profile : Configs::dataManager->profilesRepo->GetProfileBatch(m_profileIds)) {
        if (profile) m_filterKeys.insert(profile->id, makeFilterKey(profile));
    }
}

const ProfilesTableModel::FilterKey *ProfilesTableModel::filterKeyAt(int row) const {
    if (row < 0 || row >= m_profileIds.size()) return nullptr;
    ensureFilterIndex();
    auto it = m_filterKeys.constFind(m_profileIds[row]);
    return it == m_filterKeys.constEnd() ? nullptr : &it.value();
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
        // A bulk refresh can rewrite filter fields (clearing tests wipes test_country).
        m_filterKeys.clear();
        m_filterIndexBuilt = false;

        QModelIndex topLeft = index(0, 0);
        QModelIndex bottomRight = index(m_profileIds.count() - 1, columnCount() - 1);

        emit dataChanged(topLeft, bottomRight);
    }
}

void ProfilesTableModel::refreshProfileId(int profileId) {
    if (!id2row.contains(profileId)) return;
    // Keep the filter key in step before dataChanged makes the proxy re-test the row.
    if (m_filterIndexBuilt) {
        if (auto profile = Configs::dataManager->profilesRepo->GetProfile(profileId)) {
            m_filterKeys.insert(profileId, makeFilterKey(profile));
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

    // Every row between the two shifted by one; id2row has to follow.
    const int from = std::max(std::min(row1, row2), 0);
    const int to = std::min(std::max(row1, row2), static_cast<int>(m_profileIds.size()) - 1);
    for (int i = from; i <= to; ++i) id2row[m_profileIds[i]] = i;
    for (int i = from; i <= to; ++i) refreshProfileId(m_profileIds[i]);
}

void ProfilesTableModel::reorderProfiles(const QList<int> &ids) {
    if (ids.size() != m_profileIds.size()) return;
    for (int targetRow = 0; targetRow < ids.size(); ++targetRow) {
        const int sourceRow = m_profileIds.indexOf(ids[targetRow], targetRow);
        if (sourceRow < 0 || sourceRow == targetRow) continue;
        const int destinationChild = sourceRow < targetRow ? targetRow + 1 : targetRow;
        beginMoveRows({}, sourceRow, sourceRow, {}, destinationChild);
        m_profileIds.move(sourceRow, targetRow);
        endMoveRows();
    }
    id2row.clear();
    for (int row = 0; row < m_profileIds.size(); ++row) id2row.insert(m_profileIds[row], row);
}

int ProfilesTableModel::indexOfProfile(int id) {
    if (id2row.contains(id)) return id2row.value(id);
    return -1;
}

QString ProfilesTableModel::rowLabel(int sourceRow, int displayRow) const {
    if (sourceRow < 0 || sourceRow >= m_profileIds.size()) return {};
    int id = m_profileIds[sourceRow];
    if (Configs::dataManager->settingsRepo->started_id == id) {
        return QStringLiteral("✓");
    }
    return QString::number(displayRow + 1) + QStringLiteral("  ");
}
