#include <include/database/entities/Group.h>

#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"

namespace Configs
{
    void Group::clearCalculatedColumnWidth() {
        calculated_column_width.clear();
    }

    QList<int> Group::Profiles() const {
        return profiles;
    }

    bool Group::SortProfiles(GroupSortAction sortAction) {
        if (!mutex.tryLock()) {
            return false;
        }
        auto allProfs = dataManager->profilesRepo->GetProfileBatch(profiles); // to warm up the cache
        switch (sortAction.method) {
            case GroupSortMethod::Raw: {
                break;
            }
            case GroupSortMethod::ById: {
                break;
            }
            case GroupSortMethod::ByAddress:
            case GroupSortMethod::ByName:
            case GroupSortMethod::ByTestResult:
            case GroupSortMethod::ByTraffic:
            case GroupSortMethod::ByType: {
                auto get_latency_for_sort = [](const std::shared_ptr<Profile>& prof) {
                    auto i = prof->latency;
                    if (i == 0) i = 100000;
                    if (i < 0) i = 99999;
                    return i;
                };
                std::ranges::sort(profiles,
                                  [&](int a, int b) {
                                      auto profA = dataManager->profilesRepo->GetProfile(a);
                                      auto profB = dataManager->profilesRepo->GetProfile(b);
                                      QString ms_a;
                                      QString ms_b;
                                      if (sortAction.method == GroupSortMethod::ByType) {
                                          ms_a = profA->outbound->DisplayType();
                                          ms_b = profB->outbound->DisplayType();
                                      } else if (sortAction.method == GroupSortMethod::ByName) {
                                          ms_a = profA->outbound->name;
                                          ms_b = profB->outbound->name;
                                      } else if (sortAction.method == GroupSortMethod::ByAddress) {
                                          ms_a = profA->outbound->DisplayAddress();
                                          ms_b = profB->outbound->DisplayAddress();
                                      } else if (sortAction.method == GroupSortMethod::ByTestResult) {
                                          if (test_sort_by == testBy::latency) {
                                              return sortAction.descending ? get_latency_for_sort(profA) > get_latency_for_sort(profB) : get_latency_for_sort(profA) < get_latency_for_sort(profB);
                                          }
                                          if (test_sort_by == testBy::txSpeed) {
                                              return sortAction.descending ? profA->ul_speed_mbps > profB->ul_speed_mbps : profA->ul_speed_mbps < profB->ul_speed_mbps;
                                          }
                                          if (test_sort_by == testBy::rxSpeed) {
                                              return sortAction.descending ? profA->dl_speed_mbps > profB->dl_speed_mbps : profA->dl_speed_mbps < profB->dl_speed_mbps;
                                          }
                                          if (test_sort_by == testBy::connectTime) {
                                              auto connA = profA->connect_time_ms == 0 ? 100000 : profA->connect_time_ms;
                                              auto connB = profB->connect_time_ms == 0 ? 100000 : profB->connect_time_ms;
                                              if (connA < 0) connA = 99999;
                                              if (connB < 0) connB = 99999;
                                              return sortAction.descending ? connA > connB : connA < connB;
                                          }
                                          if (test_sort_by == testBy::siteScore) {
                                              if (profA->site_score == profB->site_score) {
                                                  return false;
                                              }
                                              return sortAction.descending ? profA->site_score > profB->site_score : profA->site_score < profB->site_score;
                                          }
                                      } else if (sortAction.method == GroupSortMethod::ByTraffic) {
                                          if (traffic_sort_by == trafficBy::tx) {
                                              return sortAction.descending ? profA->traffic_uplink > profB->traffic_uplink : profA->traffic_uplink < profB->traffic_uplink;
                                          }
                                          if (traffic_sort_by == trafficBy::rx) {
                                              return sortAction.descending ? profA->traffic_downlink > profB->traffic_downlink : profA->traffic_downlink < profB->traffic_downlink;
                                          }
                                      }
                                      return sortAction.descending ? ms_a > ms_b : ms_a < ms_b;
                                  });
                break;
            }
        }
        mutex.unlock();
        return true;
    }

    bool Group::AddProfile(int ID)
    {
        QMutexLocker locker(&mutex);
        if (HasProfile(ID))
        {
            return false;
        }
        profiles.append(ID);
        return true;
    }

    bool Group::AddProfileBatch(const QList<int>& IDs) {
        QSet<int> currentProfiles;
        for (const auto& profileID : profiles) {
            currentProfiles.insert(profileID);
        }
        QMutexLocker locker(&mutex);
        for (auto profileID : IDs) {
            if (!currentProfiles.contains(profileID)) {
                profiles.append(profileID);
            }
        }
        return true;
    }

    bool Group::RemoveProfile(int ID)
    {
        QMutexLocker locker(&mutex);
        if (!HasProfile(ID)) return false;
        profiles.removeAll(ID);
        return true;
    }

    bool Group::RemoveProfileBatch(const QList<int>& IDs) {
        QSet<int> toDel;
        for (auto ID : IDs) {
            toDel.insert(ID);
        }
        QList<int> newIDs;
        QMutexLocker locker(&mutex);
        for (auto inID : profiles) {
            if (!toDel.contains(inID)) {
                newIDs.append(inID);
            }
        }
        profiles = newIDs;
        return true;
    }

    bool Group::SwapProfiles(int idx1, int idx2)
    {
        QMutexLocker locker(&mutex);
        if (profiles.size() <= idx1 || profiles.size() <= idx2) return false;
        profiles.swapItemsAt(idx1, idx2);
        return true;
    }

    bool Group::EmplaceProfile(int idx, int newIdx)
    {
        QMutexLocker locker(&mutex);
        if (profiles.size() <= idx || profiles.size() <= newIdx) return false;
        profiles.insert(newIdx+1, profiles[idx]);
        if (idx < newIdx) profiles.remove(idx);
        else profiles.remove(idx+1);
        return true;
    }

    bool Group::HasProfile(int ID) const
    {
        return profiles.contains(ID);
    }
}
