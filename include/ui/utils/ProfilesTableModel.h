#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QHash>
#include <QColor>
#include <memory>

namespace Configs {
    class Profile;
}

// On-demand profile list model with configurable LRU cache.
// Holds only profile IDs; cell data is loaded via ProfilesRepo when requested.
class ProfilesTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum {
        ProfileIdRole = Qt::UserRole,
        GroupIdRole,
        GroupHeaderRole,
    };

    struct GroupSection {
        int groupId = -1;
        QList<int> profileIds;
    };

    // Column order of the proxy table. Everything that indexes the table by
    // column (sorting, header menus, saved widths, the filter header) refers to
    // these instead of raw numbers.
    enum Column {
        ColStartup = 0,
        ColDisabled,
        ColType,
        ColAddress,
        ColName,
        ColLatency,
        ColRxSpeed,
        ColConnectionTime,
        ColSiteScore,
        ColRxTraffic,
        ColTxTraffic,
        ColumnCount,
    };

    // Filterable fields, held in memory so filtering never pages profiles in one
    // at a time through the LRU cache below.
    struct FilterKey {
        QString type;
        QString address;
        QString name;
        QString country;
        int port = 0;
    };

    explicit ProfilesTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Rebuild the flat table as named group separators plus the profiles under
    // each group. The view still sees one continuous model, which lets Qt keep
    // normal Ctrl/Shift selection semantics across group boundaries.
    void setGroupSections(const QList<GroupSection> &sections);

    // Legacy single-section refresh kept for callers that only change one
    // group. New main-window code uses setGroupSections().
    void refreshTable(const QList<int> &ids = {}, bool mayNeedReset = false);

    // Invalidate one row so the view repaints (e.g. after latency/traffic update).
    void refreshProfileId(int profileId);

    void emplaceProfiles(int row1, int row2);

    // Match a newly sorted group without resetting the model or discarding the
    // profile cache and selection.
    void reorderProfiles(const QList<int> &ids);

    int indexOfProfile(int id);

    // IDs of every profile represented in the unified table, in display order.
    QList<int> allProfileIds() const { return m_profileIds; }

    bool isGroupHeader(int row) const;
    int groupIdAt(int row) const;
    QList<int> profileIdsForGroup(int groupId) const;

    // Vertical header label: "✓" for the running profile, else displayRow + 1.
    // A filter makes the two rows disagree, hence both arguments.
    QString rowLabel(int sourceRow, int displayRow) const;

    // Null if the profile could not be loaded; valid until the next model change.
    const FilterKey *filterKeyAt(int row) const;

private:
    struct Row {
        int profileId = -1;
        int groupId = -1;
        int profileCount = 0;
    };
    void ensureCached(int profileId) const;
    void evictOne() const;
    void setProfileIds(const QList<int> &ids);
    void ensureFilterIndex() const;

    QList<int> m_profileIds;
    QList<Row> m_rows;
    mutable QHash<int, int> id2row;
    mutable QHash<int, std::shared_ptr<Configs::Profile>> m_cache;
    mutable QList<int> m_lruOrder;
    // Enough for several screens while keeping thousands of subscriptions
    // virtual: rapid scrolling does not immediately evict rows just painted.
    int m_cacheSize = 512;

    mutable QHash<int, FilterKey> m_filterKeys;
    mutable bool m_filterIndexBuilt = false;
};
