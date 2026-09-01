#include "include/stats/traffic/TrafficLooper.hpp"

#include "include/api/RPC.h"
#include "include/ui/mainwindow_interface.h"

#include <QThread>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QSet>

#include "include/database/ProfilesRepo.h"
#include "include/database/GroupsRepo.h"
#include "include/database/DatabaseManager.h"
#include "include/stats/traffic/TrafficStatsManager.hpp"


namespace Stats {

    TrafficLooper *trafficLooper = new TrafficLooper;
    QElapsedTimer elapsedTimer;

    namespace {
        constexpr int kTrafficSaveIntervalSecs = 10;
    }

    void TrafficLooper::UpdateAll() {
        // Runtime counters feed the status area, graph and Info pane.  They must
        // remain available even when the user disables *persistent* traffic
        // history; otherwise a working tunnel is displayed as 0 B / 0 Mbps.
        const bool persistTraffic = !Configs::dataManager->settingsRepo->disable_traffic_stats;

        auto resp = API::defaultClient->QueryStats();
        const auto now = elapsedTimer.elapsed();

        proxy->uplink_rate = 0;
        proxy->downlink_rate = 0;
        QSet<QString> countedProxyTags;

        for (auto& group : groups) {
            const auto& tagKey = group.watchTagKey;
            if (!resp.ups.contains(tagKey)) continue;
            const auto interval = now - group.last_update;
            group.last_update = now;
            if (interval <= 0) continue;
            const auto up = resp.ups.at(tagKey);
            const auto down = resp.downs.at(tagKey);
            // An auto-selector contributes one group per pool member, all but
            // one of them idle at any moment. Skipping the zero deltas keeps a
            // 300-member pool from doing 300 no-op stat writes every second.
            if (persistTraffic && (up != 0 || down != 0)) {
                for (auto& profile : group.profiles) {
                    profile->traffic_uplink += up;
                    profile->traffic_downlink += down;
                    trafficStatsManager->AddConfigDelta(profile->id, up, down);
                }
                group.dirty = true;
            }
            group.uplink_rate = static_cast<double>(up) * 1000.0 / static_cast<double>(interval);
            group.downlink_rate = static_cast<double>(down) * 1000.0 / static_cast<double>(interval);
            // A route can expose the same watched outbound to more than one
            // profile group. Each profile still needs its own accounting above,
            // but the aggregate Proxy panel must count those core bytes once.
            if (!countedProxyTags.contains(group.watchTag)) {
                countedProxyTags.insert(group.watchTag);
                proxy->uplink_rate += group.uplink_rate;
                proxy->downlink_rate += group.downlink_rate;
                proxy->uplink_total += up;
                proxy->downlink_total += down;
            }
        }

        direct->uplink_rate = 0;
        direct->downlink_rate = 0;
        const std::string directTag = "direct";
        if (resp.ups.contains(directTag)) {
            const auto interval = now - direct_last_update;
            direct_last_update = now;
            if (interval > 0) {
                const auto up = resp.ups.at(directTag);
                const auto down = resp.downs.at(directTag);
                direct->uplink_rate = static_cast<double>(up) * 1000.0 / static_cast<double>(interval);
                direct->downlink_rate = static_cast<double>(down) * 1000.0 / static_cast<double>(interval);
                direct->uplink_total += up;
                direct->downlink_total += down;
                if (persistTraffic) {
                    trafficStatsManager->AddConfigDelta(DIRECT_STAT_PROFILE_ID, up, down);
                }
            }
        }
        proxy->max_rate = qMax(proxy->max_rate, proxy->uplink_rate + proxy->downlink_rate);
        direct->max_rate = qMax(direct->max_rate, direct->uplink_rate + direct->downlink_rate);
    }

    void TrafficLooper::Loop() {
        elapsedTimer.start();
        int secs_since_save = 0;
        while (true) {
            QThread::msleep(1000);

            // profile start and stop
            if (!loop_enabled) {
                if (looping) {
                    looping = false;
                    runOnUiThread([=] {
                        auto m = GetMainWindow();
                        m->refresh_status("STOP");
                    });
                }
                runOnUiThread([=]
                {
                   auto *m = GetMainWindow();
                   if (m == nullptr || !m->isVisible() || m->isMinimized()) return;
                   m->update_traffic_graph(0, 0, 0, 0);
                });
                continue;
            } else {
                if (!looping) {
                    looping = true;
                }
            }

            loop_mutex.lock();

            UpdateAll();

            loop_mutex.unlock();

            if (++secs_since_save >= kTrafficSaveIntervalSecs) {
                secs_since_save = 0;
                PersistTraffic();
            }

            runOnUiThread([=,this] {
                auto *m = GetMainWindow();
                if (m == nullptr) return;
                const bool foreground = m->isVisible() && !m->isMinimized();
                if (proxy != nullptr) {
                    m->refresh_status(QObject::tr("Proxy: %1\nDirect: %2").arg(DisplaySpeed(proxy), DisplaySpeed(direct)));
                    if (foreground) {
                        m->update_traffic_graph(proxy->downlink_rate, proxy->uplink_rate, direct->downlink_rate, direct->uplink_rate);
                    }
                }
                if (!foreground) return;
                // One batched refresh: a 300-member auto-selector pool would
                // otherwise fire hundreds of list refreshes every second.
                QList<int> ids;
                QSet<int> seen;
                {
                    QMutexLocker lk(&loop_mutex);
                    for (const auto& group : groups) {
                        for (const auto& profile : group.profiles) {
                            if (!profile || profile->id < 0) continue;
                            if (seen.contains(profile->id)) continue;
                            seen.insert(profile->id);
                            ids << profile->id;
                        }
                    }
                }
                if (!ids.isEmpty()) m->refresh_proxy_list(ids);
            });
        }
    }

    void TrafficLooper::PersistTraffic() {
        QList<std::shared_ptr<Configs::Profile>> all;
        {
            QMutexLocker lk(&loop_mutex);
            // A profile can appear in several groups (an auto selector is credited by every member), so dedup first.
            QSet<int> seen;
            for (auto& group : groups) {
                if (!group.dirty) continue;
                group.dirty = false;
                for (const auto& profile : group.profiles) {
                    if (!profile || profile->id < 0) continue;
                    if (seen.contains(profile->id)) continue;
                    seen.insert(profile->id);
                    all.append(profile);
                }
            }
        }
        if (all.isEmpty()) return;
        if (Configs::dataManager && Configs::dataManager->profilesRepo) {
            Configs::dataManager->profilesRepo->SaveTrafficBatch(all);
        }
    }

    void TrafficLooper::SetChainGroups(const QList<Configs::TrafficChainGroup>& configGroups) {
        // The polling thread reads these containers while collecting core
        // counters. Replacing a running configuration must be one atomic view
        // for both the graph and the Info panel.
        QMutexLocker locker(&loop_mutex);
        proxy = std::make_shared<TrafficLooperEntry>();
        proxy->tag = "proxy";
        direct = std::make_shared<TrafficLooperEntry>();
        direct->tag = "direct";

        // Seed last_update to now, or the first rate sample is divided by however long the app has been up.
        const auto now = elapsedTimer.isValid() ? elapsedTimer.elapsed() : 0;

        groups.clear();
        for (const auto& configGroup : configGroups) {
            if (configGroup.watchTag.isEmpty() || configGroup.profiles.isEmpty()) continue;
            TrafficLooperGroup g;
            g.watchTag = configGroup.watchTag;
            g.watchTagKey = configGroup.watchTag.toStdString();
            g.profiles = configGroup.profiles;
            g.last_update = now;
            groups.append(g);
        }
        direct_last_update = now;

        trafficStatsManager->EnsureDirectMeta();
        QSet<int> snapshotted;
        for (const auto& g : groups) {
            for (const auto& p : g.profiles) {
                if (!p || p->id < 0) continue;
                if (snapshotted.contains(p->id)) continue;
                snapshotted.insert(p->id);
                QString groupName;
                if (const auto grp = Configs::dataManager->groupsRepo->GetGroup(p->gid)) groupName = grp->name;
                trafficStatsManager->SnapshotConfigMeta(
                    p->id,
                    p->outbound ? p->outbound->DisplayName() : p->name,
                    groupName,
                    p->type,
                    p->outbound ? p->outbound->DisplayAddress() : QString());
            }
        }
    }

} // namespace Stats
