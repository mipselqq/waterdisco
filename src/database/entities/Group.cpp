#include <include/database/entities/Group.h>

#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"

#include <algorithm>

namespace Configs
{
    void Group::clearCalculatedColumnWidth() {
        calculated_column_width.clear();
    }

    QList<int> Group::Profiles() const {
        return profiles;
    }

    double bitrateToBps(const QString& str)
    {
        if (str.endsWith("Gbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e9;
        }
        if (str.endsWith("Mbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e6;
        }
        if (str.endsWith("Kbps", Qt::CaseInsensitive)) {
            double val = str.left(str.size() - 4).toDouble();
            return val * 1e3;
        }
        if (str == "N/A") return -1;
        return 0.0;
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
            case GroupSortMethod::BySecurity:
            case GroupSortMethod::ByType: {
                std::stable_sort(profiles.begin(), profiles.end(),
                                  [&](int a, int b) {
                                      auto profA = dataManager->profilesRepo->GetProfile(a);
                                      auto profB = dataManager->profilesRepo->GetProfile(b);
                                      if (!profA || !profB) return profA != nullptr;
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
                                      } else if (sortAction.method == GroupSortMethod::BySecurity) {
                                          auto secA = profA->outbound->GetSecurity();
                                          auto secB = profB->outbound->GetSecurity();
                                          if (secA.level != secB.level) {
                                              return sortAction.descending ? secA.level > secB.level
                                                                           : secA.level < secB.level;
                                          }
                                          ms_a = secA.transport + secA.label;
                                          ms_b = secB.transport + secB.label;
                                      } else if (sortAction.method == GroupSortMethod::ByTestResult) {
                                          const auto statusForSort = [this](const std::shared_ptr<Profile> &profile) {
                                              if (test_sort_by == testBy::latency) {
                                                  if (profile->latency > 0) return PerformanceTestStatus::Success;
                                                  if (profile->latency == 0) return PerformanceTestStatus::Untested;
                                                  return PerformanceTestStatus::Error;
                                              }
                                              return profile->performance_test_status;
                                          };
                                          const auto statusA = statusForSort(profA);
                                          const auto statusB = statusForSort(profB);

                                          double valueA = 0.0;
                                          double valueB = 0.0;
                                          if (test_sort_by == testBy::latency) {
                                              valueA = profA->latency;
                                              valueB = profB->latency;
                                          } else if (test_sort_by == testBy::txSpeed) {
                                              valueA = bitrateToBps(profA->ul_speed);
                                              valueB = bitrateToBps(profB->ul_speed);
                                          } else if (test_sort_by == testBy::rxSpeed) {
                                              valueA = profA->rx_speed_mbps;
                                              valueB = profB->rx_speed_mbps;
                                          } else if (test_sort_by == testBy::connectTime) {
                                              valueA = profA->connect_time_ms;
                                              valueB = profB->connect_time_ms;
                                          } else if (test_sort_by == testBy::siteScore) {
                                              valueA = profA->site_score;
                                              valueB = profB->site_score;
                                          }
                                          return PerformanceComesBefore(statusA, valueA, statusB, valueB,
                                                                        sortAction.descending);
                                      } else if (sortAction.method == GroupSortMethod::ByTraffic) {
                                          const qint64 valueA = traffic_sort_by == trafficBy::rx
                                              ? profA->traffic_downlink : profA->traffic_uplink;
                                          const qint64 valueB = traffic_sort_by == trafficBy::rx
                                              ? profB->traffic_downlink : profB->traffic_uplink;
                                          if (valueA == valueB) return false;
                                          return sortAction.descending ? valueA > valueB : valueA < valueB;
                                      }
                                      return sortAction.descending ? ms_a > ms_b : ms_a < ms_b;
                                  });
                break;
            }
        }
        // Disabled profiles remain visible for management, but never interrupt the
        // active portion of a group. Keep both partitions stable so manual order
        // and the selected sort order are preserved within each one.
        std::stable_partition(profiles.begin(), profiles.end(), [](int id) {
            return !dataManager->settingsRepo->IsProfileDisabled(id);
        });
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
