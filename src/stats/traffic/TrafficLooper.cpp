#include "include/stats/traffic/TrafficLooper.hpp"

#include "include/api/RPC.h"
#include "include/ui/mainwindow_interface.h"

#include <QThread>
#include <QJsonDocument>
#include <QElapsedTimer>

#include "include/database/ProfilesRepo.h"


namespace Stats {

    TrafficLooper *trafficLooper = new TrafficLooper;
    QElapsedTimer elapsedTimer;

    void TrafficLooper::UpdateAll() {
        if (Configs::dataManager->settingsRepo->disable_traffic_stats) {
            return;
        }

        auto resp = API::defaultClient->QueryStats();
        proxy->uplink_rate = 0;
        proxy->downlink_rate = 0;

        int proxyUp = 0, proxyDown = 0;
        const auto now = static_cast<long long>(elapsedTimer.elapsed());

        for (auto& e : entries) {
            const QString tag = e.tag;
            if (!resp.ups.contains(tag.toStdString())) continue;
            const auto up = resp.ups.at(tag.toStdString());
            const auto down = resp.downs.at(tag.toStdString());
            if (tag == "proxy") {
                proxyUp = up;
                proxyDown = down;
            }

            const auto interval = now - e.last_update;
            e.last_update = now;
            if (interval <= 0) continue;

            e.uplink += up;
            e.downlink += down;
            e.uplink_rate = static_cast<double>(up) * 1000.0 / static_cast<double>(interval);
            e.downlink_rate = static_cast<double>(down) * 1000.0 / static_cast<double>(interval);

            if (e.ent) {
                e.ent->traffic_downlink = e.downlink;
                e.ent->traffic_uplink = e.uplink;
                if (!e.ignore_for_rate) {
                    proxy->uplink_rate += e.uplink_rate;
                    proxy->downlink_rate += e.downlink_rate;
                }
            } else {
                direct->last_update = now;
                direct->uplink += up;
                direct->downlink += down;
                direct->uplink_rate = e.uplink_rate;
                direct->downlink_rate = e.downlink_rate;
            }
        }

        if (isChain) {
            for (auto& e : entries) {
                if (e.ent && e.is_chain_tail) {
                    e.downlink += proxyDown;
                    e.uplink += proxyUp;
                    e.ent->traffic_downlink = e.downlink;
                    e.ent->traffic_uplink = e.uplink;
                }
            }
        }
    }

    void TrafficLooper::Loop() {
        elapsedTimer.start();
        while (true) {
            QThread::msleep(1000); // refresh every one second

            if (Configs::dataManager->settingsRepo->disable_traffic_stats) {
                continue;
            }

            // profile start and stop
            if (!loop_enabled) {
                // 停止
                if (looping) {
                    looping = false;
                    runOnUiThread([=] {
                        auto m = GetMainWindow();
                        m->refresh_status("STOP");
                    });
                }
                runOnUiThread([=]
                {
                   auto m = GetMainWindow();
                   m->update_traffic_graph(0, 0, 0, 0);
                });
                continue;
            } else {
                // 开始
                if (!looping) {
                    looping = true;
                }
            }

            // do update
            loop_mutex.lock();

            UpdateAll();

            loop_mutex.unlock();

            // post to UI
            runOnUiThread([=,this] {
                auto m = GetMainWindow();
                if (proxy != nullptr) {
                    m->refresh_status(QObject::tr("Proxy: %1\nDirect: %2").arg(DisplaySpeed(proxy), DisplaySpeed(direct)));
                    m->update_traffic_graph(proxy->downlink_rate, proxy->uplink_rate, direct->downlink_rate, direct->uplink_rate);
                }
                for (const auto &profile: profiles) {
                    m->refresh_proxy_list({profile->id});
                    Configs::dataManager->profilesRepo->SaveTraffic(profile);
                }
            });
        }
    }

    void TrafficLooper::SetEnts(const QList<std::pair<std::shared_ptr<Configs::Profile>, QString>>& profsAndTags) {
        proxy = std::make_shared<TrafficLooperEntry>("proxy");
        direct = std::make_shared<TrafficLooperEntry>("direct");

        entries.clear();
        profiles.clear();
        for (const auto& [profile, tag] : profsAndTags) {
            if (tag.isEmpty()) continue;
            profiles.append(profile);
            TrafficLooperEntry e;
            e.tag = tag;
            e.ent = profile.get();
            e.downlink = profile->traffic_downlink;
            e.uplink = profile->traffic_uplink;
            entries.append(e);
        }
        entries.append(*direct.get());
    }

} // namespace Stats
