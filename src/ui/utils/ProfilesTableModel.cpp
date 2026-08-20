#include "include/ui/utils/ProfilesTableModel.h"
#include "include/global/Configs.hpp"
#include "include/database/entities/Profile.h"
#include "include/configs/common/Outbound.h"
#include <QApplication>
#include <QFontMetrics>
#include <QMimeData>
#include <QPalette>

#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"

ProfilesTableModel::ProfilesTableModel(QObject *parent)
    : QAbstractTableModel(parent) {}

int ProfilesTableModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return m_rows.size();
}

int ProfilesTableModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return ColumnCount;
}

Qt::ItemFlags ProfilesTableModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags defaultFlags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        if (isGroupHeader(index.row())) return Qt::ItemIsEnabled;
        const int profileId = m_rows.value(index.row()).profileId;
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
    // This method is called for every role of every visible cell. Moving an
    // entry through a QList on a cache hit made scrolling O(cache-size) per
    // paint. FIFO eviction is sufficient for the bounded viewport cache and
    // keeps the hot path O(1).
    if (m_cache.contains(profileId)) return;

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
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
        || index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }
    const Row &row = m_rows[index.row()];
    if (role == GroupIdRole) return row.groupId;
    if (role == GroupHeaderRole) return row.profileId < 0;
    if (row.profileId < 0) {
        if (role == Qt::DisplayRole && index.column() == ColNumber) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(row.groupId);
            const QString name = group ? group->name : tr("Unknown group");
            return name;
        }
        if (role == Qt::BackgroundRole) return QApplication::palette().color(QPalette::Button);
        if (role == Qt::FontRole) {
            QFont font = QApplication::font();
            font.setBold(true);
            return font;
        }
        return {};
    }
    const int profileId = row.profileId;
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

    // These roles are independent of profile data. Returning before the cache
    // lookup keeps painting the number and checkbox columns free of repository
    // work, which matters for a virtual table during a fast scroll.
    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColNumber:
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
    if (role == Qt::DisplayRole) {
        if (index.column() == ColNumber) return QString::number(row.displayNumber);
        if (index.column() == ColStartup || index.column() == ColDisabled) return QString();
    }
    if (role == Qt::FontRole || role == Qt::DecorationRole || role == Qt::SizeHintRole) {
        return {};
    }

    ensureCached(profileId);
    auto it = m_cache.constFind(profileId);
    if (it == m_cache.constEnd()) return {};
    const std::shared_ptr<Configs::Profile> &profile = it.value();
    if (!profile) return {};

    if (role == Qt::BackgroundRole) {
        switch (profile->connection_test_status) {
        case Configs::ConnectionTestStatus::Pending:
            return QColor(QStringLiteral("#73552f"));
        case Configs::ConnectionTestStatus::Success:
            return QColor(QStringLiteral("#315f42"));
        case Configs::ConnectionTestStatus::Error:
            return QColor(QStringLiteral("#713d3d"));
        case Configs::ConnectionTestStatus::Idle:
            break;
        }
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
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
                // Skipped is intentionally muted, but must remain readable in
                // the dense table (the disabled palette is too faint here).
                return QColor(QStringLiteral("#c5cbd2"));
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
        return {};
    }
    return {};
}

bool ProfilesTableModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() || role != Qt::CheckStateRole
        || index.row() < 0 || index.row() >= m_rows.size() || isGroupHeader(index.row())) {
        return false;
    }

    const int profileId = m_rows[index.row()].profileId;
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
        case ColNumber: return tr("№");
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
    m_rows.clear();
    m_rows.reserve(ids.size());
    for (const auto &id : ids) {
        m_rows.append({id, -1, 0});
    }
    rebuildRowIndexesAndNumbers();
    m_cache.clear();
    m_lruOrder.clear();
    m_filterKeys.clear();
    m_filterIndexBuilt = false;
    invalidatePreferredColumnWidths();
    endResetModel();
}

bool ProfilesTableModel::setGroupSections(const QList<GroupSection> &sections, bool forceRefresh) {
    int expectedRows = 0;
    for (const GroupSection &section : sections) expectedRows += 1 + section.profileIds.size();
    bool sameStructure = expectedRows == m_rows.size();
    if (sameStructure) {
        int row = 0;
        for (const GroupSection &section : sections) {
            if (m_rows[row].profileId != -1 || m_rows[row].groupId != section.groupId
                || m_rows[row].profileCount != section.profileIds.size()) {
                sameStructure = false;
                break;
            }
            ++row;
            for (int id : section.profileIds) {
                if (m_rows[row].profileId != id || m_rows[row].groupId != section.groupId) {
                    sameStructure = false;
                    break;
                }
                ++row;
            }
            if (!sameStructure) break;
        }
    }
    if (sameStructure) {
        if (forceRefresh && !m_rows.isEmpty()) {
            emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColumnCount - 1));
        }
        return false;
    }

    beginResetModel();
    m_profileIds.clear();
    m_rows.clear();
    for (const GroupSection &section : sections) {
        m_rows.append({-1, section.groupId, static_cast<int>(section.profileIds.size())});
        for (int id : section.profileIds) {
            m_profileIds.append(id);
            m_rows.append({id, section.groupId, 0});
        }
    }
    rebuildRowIndexesAndNumbers();
    m_cache.clear();
    m_lruOrder.clear();
    m_filterKeys.clear();
    m_filterIndexBuilt = false;
    invalidatePreferredColumnWidths();
    endResetModel();
    return true;
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
    if (row < 0 || row >= m_rows.size() || isGroupHeader(row)) return nullptr;
    ensureFilterIndex();
    auto it = m_filterKeys.constFind(m_rows[row].profileId);
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

        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0, 0), index(m_rows.count() - 1, columnCount() - 1));
        }
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
    if (m_rows.size() <= row1 || m_rows.size() <= row2
        || isGroupHeader(row1) || isGroupHeader(row2)
        || m_rows[row1].groupId != m_rows[row2].groupId) return;
    const Row moved = m_rows.takeAt(row1);
    const int insertAt = row1 < row2 ? row2 : row2 + 1;
    m_rows.insert(insertAt, moved);

    rebuildRowIndexesAndNumbers();
    emit dataChanged(index(0, 0), index(m_rows.size() - 1, columnCount() - 1));
}

void ProfilesTableModel::reorderProfiles(const QList<int> &ids) {
    if (ids.isEmpty()) return;
    const int groupId = groupIdAt(indexOfProfile(ids.first()));
    QList<int> current = profileIdsForGroup(groupId);
    if (current.size() != ids.size()) return;
    for (int target = 0; target < ids.size(); ++target) {
        const int currentRow = indexOfProfile(ids[target]);
        const int targetRow = indexOfProfile(current[target]);
        if (currentRow < 0 || targetRow < 0 || currentRow == targetRow) continue;
        const Row moved = m_rows.takeAt(currentRow);
        m_rows.insert(currentRow < targetRow ? targetRow : targetRow + 1, moved);
        current = profileIdsForGroup(groupId);
    }
    rebuildRowIndexesAndNumbers();
    emit layoutChanged();
}

int ProfilesTableModel::indexOfProfile(int id) {
    if (id2row.contains(id)) return id2row.value(id);
    return -1;
}

QString ProfilesTableModel::rowLabel(int sourceRow, int displayRow) const {
    if (sourceRow < 0 || sourceRow >= m_rows.size() || isGroupHeader(sourceRow)) return {};
    Q_UNUSED(displayRow)
    return QString::number(m_rows[sourceRow].displayNumber) + QStringLiteral("  ");
}

void ProfilesTableModel::rebuildRowIndexesAndNumbers() {
    id2row.clear();
    m_profileIds.clear();
    int displayNumber = 0;
    for (int row = 0; row < m_rows.size(); ++row) {
        Row &entry = m_rows[row];
        if (entry.profileId < 0) {
            entry.displayNumber = 0;
            continue;
        }
        entry.displayNumber = ++displayNumber;
        id2row.insert(entry.profileId, row);
        m_profileIds.append(entry.profileId);
    }
}

void ProfilesTableModel::invalidatePreferredColumnWidths() {
    m_preferredColumnWidths.clear();
    m_preferredWidthsFont = {};
}

QList<int> ProfilesTableModel::preferredColumnWidths(const QFont &font) const {
    if (m_preferredColumnWidths.size() == ColumnCount && m_preferredWidthsFont == font) {
        return m_preferredColumnWidths;
    }

    const QFontMetrics metrics(font);
    constexpr int cellPadding = 16;
    QList<int> widths(ColumnCount, 0);
    const auto addText = [&widths, &metrics, cellPadding](int column, const QString &text) {
        int width = 0;
        for (const QString &line : text.split('\n')) {
            width = qMax(width, metrics.horizontalAdvance(line));
        }
        widths[column] = qMax(widths[column], width + cellPadding);
    };

    for (int column = 0; column < ColumnCount; ++column) {
        addText(column, headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
    }
    widths[ColNumber] = qMax(widths[ColNumber], metrics.horizontalAdvance(QString::number(m_profileIds.size())) + cellPadding);
    widths[ColStartup] = qMax(widths[ColStartup], 48);
    widths[ColDisabled] = qMax(widths[ColDisabled], 42);

    for (const auto &profile : Configs::dataManager->profilesRepo->GetProfileBatch(m_profileIds)) {
        if (!profile) continue;
        if (profile->outbound) {
            QString type = profile->outbound->DisplayType();
            if (Configs::dataManager->settingsRepo->show_config_security) {
                const QString security = profile->outbound->DisplaySecurity();
                if (!security.isEmpty()) type += QStringLiteral(" (%1)").arg(security);
            }
            addText(ColType, type);
            addText(ColAddress, profile->outbound->DisplayAddress());
            addText(ColName, profile->outbound->name);
        }
        addText(ColLatency, profile->DisplayLatency());
        addText(ColRxSpeed, profile->DisplayRxSpeed());
        addText(ColConnectionTime, profile->DisplayConnectionTime());
        addText(ColSiteScore, profile->DisplaySiteScore());
        addText(ColRxTraffic, profile->DisplayTrafficRx());
        addText(ColTxTraffic, profile->DisplayTrafficTx());
    }

    // Test/traffic values can grow after the first render. Keep those columns
    // stable without rescanning them on every live update.
    addText(ColRxSpeed, QStringLiteral("9999.99 MB/s"));
    addText(ColConnectionTime, QStringLiteral("00:00:00"));
    addText(ColSiteScore, QStringLiteral("100.0"));
    addText(ColRxTraffic, QStringLiteral("999.99 GB"));
    addText(ColTxTraffic, QStringLiteral("999.99 GB"));

    m_preferredWidthsFont = font;
    m_preferredColumnWidths = widths;
    return m_preferredColumnWidths;
}

bool ProfilesTableModel::isGroupHeader(int row) const {
    return row >= 0 && row < m_rows.size() && m_rows[row].profileId < 0;
}

int ProfilesTableModel::groupIdAt(int row) const {
    return row >= 0 && row < m_rows.size() ? m_rows[row].groupId : -1;
}

QList<int> ProfilesTableModel::profileIdsForGroup(int groupId) const {
    QList<int> ids;
    for (const Row &row : m_rows) {
        if (row.groupId == groupId && row.profileId >= 0) ids.append(row.profileId);
    }
    return ids;
}
