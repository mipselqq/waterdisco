#pragma once

#include <QAbstractTableModel>
#include <QFont>
#include <QList>
#include <QHash>
#include <QColor>
#include <memory>

namespace Configs {
    class Profile;
}

// On-demand profile list model with a bounded viewport cache.
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
        ColNumber = 0,
        ColStartup,
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
    // Returns true when profile order/section membership changed. A forced
    // refresh updates group captions without invalidating the display caches.
    bool setGroupSections(const QList<GroupSection> &sections, bool forceRefresh = false,
                          bool forceReset = false);

    // Omit group header rows. Membership is unchanged; only the separators go.
    void setFlatList(bool flat);
    bool isFlatList() const { return m_flatList; }

    // Flat-mode display order that may intermix groups after a global sort.
    // Cleared when membership changes or the user leaves list mode.
    void setGlobalOrder(const QList<int> &ids);
    void clearGlobalOrder();

    // Legacy single-section refresh kept for callers that only change one
    // group. New main-window code uses setGroupSections().
    void refreshTable(const QList<int> &ids = {}, bool mayNeedReset = false);

    // Invalidate one row so the view repaints (e.g. after latency/traffic update).
    void refreshProfileId(int profileId);

    void emplaceProfiles(int row1, int row2);

    // Match a newly sorted group without resetting the model or discarding the
    // profile cache and selection.
    void reorderProfiles(const QList<int> &ids);

    // destRow is the final source row of the moved profile. Uses beginMoveRows
    // so a speedtest result does not reset the table.
    bool moveProfileToRow(int profileId, int destRow);

    int indexOfProfile(int id) const;

    // IDs of every profile represented in the unified table, in display order.
    QList<int> allProfileIds() const { return m_profileIds; }

    bool isGroupHeader(int row) const;
    int groupIdAt(int row) const;
    QList<int> profileIdsForGroup(int groupId) const;

    // Legacy vertical-header label. The main table now renders a dedicated
    // number column, but this remains for compatibility with old callers.
    QString rowLabel(int sourceRow, int displayRow) const;

    // Null if the profile could not be loaded; valid until the next model change.
    const FilterKey *filterKeyAt(int row) const;

    // Global content widths are measured once per table data/font revision.
    // The view must never ask QHeaderView to inspect only visible rows while
    // scrolling, or widths would jump as virtual rows enter the viewport.
    QList<int> preferredColumnWidths(const QFont &font) const;

private:
    struct Row {
        int profileId = -1;
        int groupId = -1;
        int profileCount = 0;
        int displayNumber = 0;
    };
    void ensureCached(int profileId) const;
    void evictOne() const;
    void setProfileIds(const QList<int> &ids);
    void ensureFilterIndex() const;
    void rebuildRowIndexesAndNumbers();
    void invalidatePreferredColumnWidths();
    QList<Row> buildRows(const QList<GroupSection> &sections) const;
    bool applyRows(const QList<Row> &newRows, bool forceRefresh, bool forceReset = false);

    QList<int> m_profileIds;
    QList<Row> m_rows;
    QList<GroupSection> m_sections;
    QList<int> m_globalOrder;
    bool m_flatList = false;
    mutable QHash<int, int> id2row;
    mutable QHash<int, std::shared_ptr<Configs::Profile>> m_cache;
    mutable QList<int> m_lruOrder;
    // Enough for several screens while keeping thousands of subscriptions
    // virtual: rapid scrolling does not immediately evict rows just painted.
    int m_cacheSize = 512;

    mutable QHash<int, FilterKey> m_filterKeys;
    mutable bool m_filterIndexBuilt = false;
    mutable QList<int> m_preferredColumnWidths;
    mutable QFont m_preferredWidthsFont;
};
