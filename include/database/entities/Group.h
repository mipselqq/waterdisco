#pragma once
#include <QList>
#include <QMutex>
#include <QString>

#include "include/ui/group/GroupSort.hpp"

namespace Configs
{
    enum class testBy : int {
        latency = 0,
        txSpeed,
        rxSpeed,
        connectTime,
        siteScore
    };

    enum class testShowItems : int {
        all = 0,
        none,
        ipOnly,
        speedOnly
    };

    enum class trafficBy : int {
        tx = 0,
        rx
    };

    enum class typeBy : int {
        byType = 0,
        bySecurity
    };

    class Group {
    public:
        QMutex mutex;
        int id = -1;
        bool archive = false;
        bool skip_auto_update = false;
        bool auto_clear_unavailable = false;
        QString name = "";
        QString url = "";
        QString info = "";
        qint64 sub_last_update = 0;
        int front_proxy_id = -1;
        int landing_proxy_id = -1;

        // list ui
        QList<int> column_width;
        QList<int> calculated_column_width; // memory only, no need to save to db
        QList<int> profiles;
        int scroll_last_profile = -1;
        testBy test_sort_by = testBy::siteScore;
        trafficBy traffic_sort_by = trafficBy::tx;
        typeBy type_sort_by = typeBy::byType;
        testShowItems test_items_to_show = testShowItems::all;
        // Memory only, not persisted. Pairs of (profileID, row as displayed).
        QList<std::pair<int, int>> selectedProfilesIdIdxPairs;

        Group() = default;

        void clearCalculatedColumnWidth();

        [[nodiscard]] QList<int> Profiles() const;

        bool SortProfiles(GroupSortAction method);

        // Reorder without changing membership. Used by live table moves so the
        // UI thread never waits on SortProfiles' full stable_sort.
        bool ReplaceProfiles(const QList<int> &ids);

        bool RemoveProfile(int ID);

        bool RemoveProfileBatch(const QList<int>& IDs);

        bool AddProfile(int ID);

        bool AddProfileBatch(const QList<int>& IDs);

        bool SwapProfiles(int idx1, int idx2);

        bool EmplaceProfile(int idx, int newIdx);

        [[nodiscard]] bool HasProfile(int ID) const;
    };

    // Shared by header-click sorts and the per-result live insert. Disabled
    // profiles are ordered separately via stable_partition in SortProfileIdList.
    [[nodiscard]] bool ProfileIdComesBefore(int idA, int idB, const GroupSortAction &action,
                                            testBy testSortBy, trafficBy trafficSortBy);

    void SortProfileIdList(QList<int> &ids, const GroupSortAction &action,
                           testBy testSortBy, trafficBy trafficSortBy);
}// namespace Configs
