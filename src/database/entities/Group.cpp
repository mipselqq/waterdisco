#include <include/database/entities/Group.h>

#include "include/database/entities/Profile.h"
#include "include/database/entities/ProfileMetrics.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"
#include <QRegularExpression> 

#include <algorithm>

namespace Configs
{
    QJsonObject SubUserInfo::toJson() const {
        QJsonObject json;
        if (!valid) return json;
        json["valid"] = valid;
        json["has_quota"] = has_quota;
        if (upload > 0) json["upload"] = upload;
        if (download > 0) json["download"] = download;
        if (total > 0) json["total"] = total;
        if (expire > 0) json["expire"] = expire;
        if (!title.isEmpty()) json["title"] = title;
        if (!web_url.isEmpty()) json["web_url"] = web_url;
        if (!support_url.isEmpty()) json["support_url"] = support_url;
        if (!announce.isEmpty()) json["announce"] = announce;
        if (server_interval > 0) json["server_interval"] = server_interval;
        return json;
    }

    SubUserInfo SubUserInfo::fromJson(const QJsonObject &json) {
        SubUserInfo res;
        if (json.isEmpty()) return res;
        res.valid = json["valid"].toBool(false);
        res.has_quota = json["has_quota"].toBool(false);
        res.upload = json["upload"].toVariant().toLongLong();
        res.download = json["download"].toVariant().toLongLong();
        res.total = json["total"].toVariant().toLongLong();
        res.expire = json["expire"].toVariant().toLongLong();
        res.title = json["title"].toString();
        res.web_url = json["web_url"].toString();
        res.support_url = json["support_url"].toString();
        res.announce = json["announce"].toString();
        res.server_interval = json["server_interval"].toInt(0);
        return res;
    }

    SubUserInfo ParseSubUserInfo(const QString &info) {
        SubUserInfo result;
        static const QRegularExpression re(R"(\b(upload|download|total|expire)\s*=\s*(\d+))", QRegularExpression::CaseInsensitiveOption);
        for (auto it = re.globalMatch(info); it.hasNext();) {
            const auto match = it.next();
            const QStringView key = match.capturedView(1);
            const qint64 value = match.capturedView(2).toLongLong();
            const auto is = [key](QStringView name) { return key.compare(name, Qt::CaseInsensitive) == 0; };
            if (is(u"upload")) {
                result.upload = value;
            } else if (is(u"download")) {
                result.download = value;
            } else if (is(u"total")) {
                result.total = value;
                result.has_quota = true;
            } else {
                result.expire = value > 1000000000000LL ? value / 1000 : value;
            }
            result.valid = true;
        }
        return result;
    }

    QJsonObject SubscriptionOptions::ToJson() const {
        QJsonObject json;
        if (!user_agent.isEmpty()) json["user_agent"] = user_agent;
        if (tls_version) json["tls_version"] = static_cast<int>(*tls_version);
        if (http_version) json["http_version"] = static_cast<int>(*http_version);
        if (send_hwid != sendHwid::keepDefault) json["send_hwid"] = static_cast<int>(send_hwid);
        if (!hwid.isEmpty()) json["hwid"] = hwid;
        if (!hwid_os.isEmpty()) json["hwid_os"] = hwid_os;
        if (!hwid_os_version.isEmpty()) json["hwid_os_version"] = hwid_os_version;
        if (!hwid_model.isEmpty()) json["hwid_model"] = hwid_model;
        if (update_interval > 0) json["update_interval"] = update_interval;
        if (respect_server_interval) json["respect_server_interval"] = *respect_server_interval;
        if (keep_working) json["keep_working"] = true;
        if (remove_duplicates) json["remove_duplicates"] = true;
        if (remove_insecure) json["remove_insecure"] = true;
        if (remove_invalid) json["remove_invalid"] = true;
        if (url_test) json["url_test"] = true;
        if (remove_unavailable) json["remove_unavailable"] = true;
        if (sort_by_latency) json["sort_by_latency"] = true;
        return json;
    }

    SubscriptionOptions SubscriptionOptions::FromJson(const QJsonObject &json) {
        SubscriptionOptions options;
        options.user_agent = json["user_agent"].toString();
        if (const int tls = json["tls_version"].toInt(-1); tls >= 0 && tls <= static_cast<int>(subTlsVersion::tls13)) {
            options.tls_version = static_cast<subTlsVersion>(tls);
        }
        if (const int http = json["http_version"].toInt(-1); http >= 0 && http <= static_cast<int>(subHttpVersion::http11)) {
            options.http_version = static_cast<subHttpVersion>(http);
        }
        const int mode = json["send_hwid"].toInt();
        if (mode == static_cast<int>(sendHwid::on) || mode == static_cast<int>(sendHwid::off)) {
            options.send_hwid = static_cast<sendHwid>(mode);
        }
        options.hwid = json["hwid"].toString();
        options.hwid_os = json["hwid_os"].toString();
        options.hwid_os_version = json["hwid_os_version"].toString();
        options.hwid_model = json["hwid_model"].toString();
        options.update_interval = std::max(json["update_interval"].toInt(), 0);
        if (json["respect_server_interval"].isBool()) options.respect_server_interval = json["respect_server_interval"].toBool();
        options.keep_working = json["keep_working"].toBool();
        options.remove_duplicates = json["remove_duplicates"].toBool();
        options.remove_insecure = json["remove_insecure"].toBool();
        options.remove_invalid = json["remove_invalid"].toBool();
        options.url_test = json["url_test"].toBool();
        options.remove_unavailable = json["remove_unavailable"].toBool();
        options.sort_by_latency = json["sort_by_latency"].toBool();
        return options;
    }

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

    bool ProfileIdComesBefore(int idA, int idB, const GroupSortAction &sortAction,
                              testBy testSortBy, trafficBy trafficSortBy) {
        auto profA = dataManager->profilesRepo->GetProfile(idA);
        auto profB = dataManager->profilesRepo->GetProfile(idB);
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
            const auto statusForSort = [testSortBy](const std::shared_ptr<Profile> &profile) {
                if (testSortBy == testBy::latency) {
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
            if (testSortBy == testBy::latency) {
                valueA = profA->latency;
                valueB = profB->latency;
            } else if (testSortBy == testBy::txSpeed) {
                valueA = bitrateToBps(profA->ul_speed);
                valueB = bitrateToBps(profB->ul_speed);
            } else if (testSortBy == testBy::rxSpeed) {
                valueA = profA->rx_speed_mbps;
                valueB = profB->rx_speed_mbps;
            } else if (testSortBy == testBy::connectTime) {
                valueA = profA->connect_time_ms;
                valueB = profB->connect_time_ms;
            } else if (testSortBy == testBy::siteScore) {
                valueA = profA->site_score;
                valueB = profB->site_score;
            }
            return PerformanceComesBefore(statusA, valueA, statusB, valueB,
                                          sortAction.descending);
        } else if (sortAction.method == GroupSortMethod::ByTraffic) {
            const qint64 valueA = trafficSortBy == trafficBy::rx
                ? profA->traffic_downlink : profA->traffic_uplink;
            const qint64 valueB = trafficSortBy == trafficBy::rx
                ? profB->traffic_downlink : profB->traffic_uplink;
            if (valueA == valueB) return false;
            return sortAction.descending ? valueA > valueB : valueA < valueB;
        }
        return sortAction.descending ? ms_a > ms_b : ms_a < ms_b;
    }

    void SortProfileIdList(QList<int> &ids, const GroupSortAction &sortAction,
                           testBy testSortBy, trafficBy trafficSortBy) {
        dataManager->profilesRepo->GetProfileBatch(ids);
        switch (sortAction.method) {
            case GroupSortMethod::Raw:
            case GroupSortMethod::ById:
                break;
            case GroupSortMethod::ByAddress:
            case GroupSortMethod::ByName:
            case GroupSortMethod::ByTestResult:
            case GroupSortMethod::ByLatency:
            case GroupSortMethod::ByTraffic:
            case GroupSortMethod::BySecurity:
            case GroupSortMethod::ByType:
                std::stable_sort(ids.begin(), ids.end(),
                                 [&](int a, int b) {
                                     return ProfileIdComesBefore(a, b, sortAction, testSortBy, trafficSortBy);
                                 });
                break;
        }
        std::stable_partition(ids.begin(), ids.end(), [](int id) {
            return !dataManager->settingsRepo->IsProfileDisabled(id);
        });
    }

    bool Group::SortProfiles(GroupSortAction sortAction) {
        if (!mutex.tryLock()) {
            return false;
        }
        SortProfileIdList(profiles, sortAction, test_sort_by, traffic_sort_by);
        mutex.unlock();
        return true;
    }

    bool Group::ReplaceProfiles(const QList<int> &ids) {
        QMutexLocker locker(&mutex);
        profiles = ids;
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
