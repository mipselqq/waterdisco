#include "include/ui/mainwindow.h"

#include "include/ui/mainWindow/TestRunner.h"

#include "include/stats/traffic/TrafficLooper.hpp"
#include "include/stats/traffic/TrafficStatsManager.hpp"
#include "include/stats/autoselector/AutoSelectorMonitor.hpp"
#include "include/configs/AutoSelectorPlan.h"
#include "include/api/RPC.h"
#include "include/ui/utils/MessageBoxTimer.h"

#include <QPushButton>
#include <QMessageBox>
#include <QJsonDocument>
#include <QFile>
#include <QRegularExpression>

#include "include/configs/generate.h"
#include "include/configs/common/xrayStreamSetting.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"

#include "include/sys/Process.hpp"

#include <algorithm>
#include <memory>

using namespace API;

void MainWindow::setup_rpc(QLocalSocket *socket) {
    // The Client is never recreated, only its connection swapped, so worker threads
    // holding `defaultClient` never touch freed memory.
    defaultClient->Reconnect(socket);

    // Loopers run for the lifetime of the app, start only once
    if (!rpc_started) {
        rpc_started = true;
        runOnNewThread([=, this] { Stats::trafficLooper->Loop(); });
        runOnNewThread([=, this] { Stats::connection_lister->Loop(); });
        runOnNewThread([=, this] { Stats::autoSelectorMonitor->Loop(); });
    }
}

bool MainWindow::set_system_dns(bool set, bool save_set) {
    if (!Configs::dataManager->settingsRepo->enable_dns_server) {
        MW_show_log(tr("You need to enable hijack DNS server first"));
        return false;
    }
    if (!get_elevated_permissions(ExitReason::RestartWithDns)) {
        return false;
    }
    bool rpcOK;
    QString res;
    if (set) {
        res = defaultClient->SetSystemDNS(&rpcOK, false);
    } else {
        res = defaultClient->SetSystemDNS(&rpcOK, true);
    }
    if (!rpcOK) {
        MW_show_log(tr("Failed to set system dns: ") + res);
        return false;
    }
    if (save_set) Configs::dataManager->settingsRepo->system_dns_set = set;
    return true;
}

int MainWindow::get_profile_to_start() {
    const auto isAvailable = [](int id) {
        if (id < 0 || Configs::dataManager->settingsRepo->IsProfileDisabled(id)) return false;
        const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
        if (!profile) return false;
        const auto group = Configs::dataManager->groupsRepo->GetGroup(profile->gid);
        return group && !group->archive;
    };
    const auto ents = get_now_selected_list();
    if (ents.size() == 1 && isAvailable(ents.first())) {
        return ents.first();
    }
    if (ents.isEmpty()) {
        if (isAvailable(last_running_profile_id)) {
            return last_running_profile_id;
        }
        const int rememberId = Configs::dataManager->settingsRepo->remember_id;
        if (isAvailable(rememberId)) {
            return rememberId;
        }
        const auto currentGroup = Configs::dataManager->groupsRepo->CurrentGroup();
        if (currentGroup) {
            const auto profiles = currentGroup->Profiles();
            for (int id : profiles) {
                if (isAvailable(id)) return id;
            }
        }
    }
    return -1;
}

bool MainWindow::handleXrayGeoAssetError(const QString& error, const QString& contextName) {
    // Two geoip/geosite failures surface here: the .dat is not installed ("failed to
    // open geoip.dat"), or it lacks the category ("failed to load code cn ...: EOF").
    const bool refGeoip = error.contains("geoip.dat");
    const bool refGeosite = error.contains("geosite.dat");
    if (!refGeoip && !refGeosite) return false;

    runOnUiThread([=, this] {
        // A batch test can raise this for many profiles at once — only act once.
        if (m_xrayGeoAssetBusy) return;
        m_xrayGeoAssetBusy = true;
        // Small delay so any in-flight UI teardown (e.g. Connecting -> idle)
        // settles before the modal prompt appears.
        setTimeout([=, this] {
            const QString base = Configs::GetBasePath();
            const bool haveGeoip = QFile::exists(base + "/geoip.dat");
            const bool haveGeosite = QFile::exists(base + "/geosite.dat");

            const bool geoipLacksCategory = refGeoip && haveGeoip;
            const bool geositeLacksCategory = refGeosite && haveGeosite;
            if (geoipLacksCategory || geositeLacksCategory) {
                const QString whichFile = geositeLacksCategory ? "geosite.dat" : "geoip.dat";
                const QString ruleType = geositeLacksCategory ? "geosite" : "geoip";

                QString category;
                QRegularExpression re(QStringLiteral("code\\s+(\\S+)\\s+from"));
                const auto m = re.match(error);
                if (m.hasMatch()) category = m.captured(1);
                const QString needed = category.isEmpty()
                    ? tr("a required category")
                    : QStringLiteral("%1:%2").arg(ruleType, category);

                MessageBoxWarning(
                    tr("Geo asset missing category"),
                    tr("The Xray config \"%1\" needs \"%2\", but the installed %3 does "
                       "not contain it.\n\n"
                       "Re-downloading from the same source will not fix this — the data "
                       "file does not include that category. Set the GeoIP/GeoSite asset "
                       "URL in Settings to a source that provides \"%2\", then delete %3 "
                       "from the app folder and download it again.")
                        .arg(contextName, needed, whichFile));
                m_xrayGeoAssetBusy = false;
                return;
            }

            // Case 1: the referenced asset file is missing -> offer to download it.
            if (QMessageBox::question(this, tr("Geo asset files required"),
                    tr("The Xray config \"%1\" uses geoip/geosite routing rules, but the "
                       "required data files (geoip.dat / geosite.dat) are not installed.\n\n"
                       "Download them now?").arg(contextName)) != QMessageBox::Yes) {
                m_xrayGeoAssetBusy = false;
                return;
            }

            runOnNewThread([=, this] {
                QString dlErr;
                if (!haveGeoip) {
                    auto e = NetworkRequestHelper::DownloadAsset(Configs::dataManager->settingsRepo->xray_geoip_url, "geoip.dat");
                    if (!e.isEmpty()) dlErr += "geoip.dat: " + e + "\n";
                }
                if (!haveGeosite) {
                    auto e = NetworkRequestHelper::DownloadAsset(Configs::dataManager->settingsRepo->xray_geosite_url, "geosite.dat");
                    if (!e.isEmpty()) dlErr += "geosite.dat: " + e + "\n";
                }
                runOnUiThread([=, this] {
                    m_xrayGeoAssetBusy = false;
                    if (!dlErr.isEmpty()) {
                        MessageBoxWarning(tr("Geo asset download failed"), dlErr);
                    } else {
                        MW_show_log(tr("Downloaded Xray geo asset files."));
                        QMessageBox::information(this, tr("Geo assets installed"),
                            tr("Geo data files were downloaded successfully.\n\n"
                               "Please try again."));
                    }
                });
            });
        }, this, 300);
    });
    return true;
}

void MainWindow::profile_start(int _id) {
    if (Configs::dataManager->settingsRepo->prepare_exit) return;
#ifdef Q_OS_LINUX
    if (Configs::dataManager->settingsRepo->enable_dns_server && Configs::dataManager->settingsRepo->dns_server_listen_port <= 1024) {
        if (!get_elevated_permissions()) {
            MW_show_log(QString("Failed to get admin access, cannot listen on port %1 without it").arg(Configs::dataManager->settingsRepo->dns_server_listen_port));
            return;
        }
    }
#endif

    std::shared_ptr<Configs::Profile> ent = nullptr;
    if (_id >= 0) {
        ent = Configs::dataManager->profilesRepo->GetProfile(_id);
    } else {
        const int startId = get_profile_to_start();
        if (startId >= 0) {
            ent = Configs::dataManager->profilesRepo->GetProfile(startId);
        }
    }
    if (ent == nullptr) return;
    if (Configs::dataManager->settingsRepo->IsProfileDisabled(ent->id)) return;

    last_running_profile_id = ent->id;

    if (select_mode) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }

    const auto group = Configs::dataManager->groupsRepo->GetGroup(ent->gid);
    if (group == nullptr || group->archive) return;

    // A selector with more candidates than it can run must rank before the config is
    // built, and measuring blocks - so hop off the UI thread and come back to start.
    if (ent->type == "autoselector" && !auto_selector_ranked) {
        const auto plan = Configs::PlanAutoSelector(ent);
        if (plan.error.isEmpty() && plan.needsRanking) {
            auto_selector_ranked = true;
            const int startId = ent->id;
            runOnNewThread([=, this] {
                rank_auto_selector(ent);
                runOnUiThread([=, this] {
                    auto_selector_ranked = false;
                    profile_start(startId);
                });
            });
            return;
        }
    }
    auto_selector_ranked = false;

    const auto result = Configs::BuildSingBoxConfig(ent);
    if (!result->error.isEmpty()) {
        MessageBoxWarning(tr("BuildConfig return error"), result->error);
        return;
    }

    auto profile_start_stage2 = [=, this] {
        libcore::LoadConfigReq req;
        req.core_config = QJsonObject2QString(result->coreConfig, true).toStdString();
        req.tun_ipv4_cidr = result->tunIPv4CIDR.toStdString();
        req.disable_stats = Configs::dataManager->settingsRepo->disable_traffic_stats;
        req.xray_config = QJsonObject2QString(result->xrayConfig, true).toStdString();
        req.need_xray = !result->xrayConfig.isEmpty();
        for (const auto &full : result->xrayFullConfigs) req.xray_full_configs.push_back(full.toStdString());
        if (req.need_xray || !req.xray_full_configs.empty()) {
            // Resolution is wired in the core (ThroneWiring), not baked into the config: point
            // it at sing-box's loopback DNS-in. Test instances leave these empty.
            req.xray_outbound_dns_address = ("127.0.0.1:" + QString::number(Configs::dataManager->settingsRepo->core_dns_in_port)).toStdString();
            req.xray_outbound_dns_strategy = Configs::getXrayOutboundDomainStrategy().toStdString();
            if (auto selector = ent->AutoSelector(); selector != nullptr) {
                // A pool's Xray members may be probe-only, so let the sidecar stay cold between
                // dials. The idle window must outlast the probe interval or it restarts every round.
                req.xray_lazy_start = true;
                req.xray_idle_seconds = std::max(120, selector->intervalSec * 2);
                // Resident on purpose: recycling would only put an instance build in
                // front of every failover.
                req.xray_full_idle_seconds = 0;
            }
        }
        if (!result->extraCoreData->path.isEmpty())
        {
            req.need_extra_process = true;
            req.extra_process_path = result->extraCoreData->path.toStdString();
            req.extra_process_args = result->extraCoreData->args.toStdString();
            req.extra_process_conf = result->extraCoreData->config.toStdString();
            req.extra_no_out = result->extraCoreData->noLog;
        }
        bool rpcOK;
        const QString error = defaultClient->Start(&rpcOK, req);
        if (!rpcOK) {
            return false;
        }
        if (!error.isEmpty()) {
            // Fail now and let handleXrayGeoAssetError prompt asynchronously; blocking to
            // download would trip the "no response" restart prompt. Starting again picks them up.
            if (handleXrayGeoAssetError(error, ent->outbound->DisplayTypeAndName())) {
                return false;
            }
            if (error.contains("Fwpm", Qt::CaseInsensitive)) {
                runOnUiThread([=, this] {
                    MessageBoxWarning(
                        tr("Strict routing unavailable"),
                        tr("Windows could not enable strict routing. Open Tun Settings, "
                           "disable Strict Route, and start the profile again.\n\n"
                           "Disabling Strict Route may cause DNS leaks.\n\nError: %1").arg(error));
                });
                return false;
            }
            if (error.contains("configure tun interface")) {
                runOnUiThread([=, this] {

                    QMessageBox msg(
                        QMessageBox::Information,
                        tr("Tun device misbehaving"),
                        tr("If you have trouble starting VPN, you can force reset Core process here and then try starting the profile again. The error is %1").arg(error),
                        QMessageBox::NoButton,
                        this
                    );
                    msg.addButton(tr("Reset"), QMessageBox::ActionRole);
                    auto cancel = msg.addButton(tr("Cancel"), QMessageBox::ActionRole);

                    msg.setDefaultButton(cancel);
                    msg.setEscapeButton(cancel);

                    int r = msg.exec() - 2;
                    if (r == 0) {
                        StopVPNProcess();
                    }
                });
                return false;
            }
            runOnUiThread([=, this] { MessageBoxWarning("LoadConfig return error", error); });
            return false;
        }
        Stats::trafficLooper->SetChainGroups(result->chainGroups);
        Stats::trafficLooper->loop_enabled = true;
        Stats::connection_lister->suspend = false;
        Stats::autoSelectorMonitor->SetBuild(result->autoSelectors);
        if (!result->autoSelectors.isEmpty()) {
            const auto& info = result->autoSelectors.first();
            if (auto selector = ent->AutoSelector(); selector != nullptr) {
                QList<int> builtIDs;
                QHash<int, QString> names;
                for (const auto& [tag, member] : info.members) {
                    if (member == nullptr) continue;
                    builtIDs << member->id;
                    names.insert(member->id, member->outbound ? member->outbound->DisplayName() : member->name);
                }
                const auto now = QDateTime::currentSecsSinceEpoch();
                selector->lastBuilt = builtIDs;
                selector->lastBuiltAt = now;
                selector->RecordHistory(builtIDs, names, now);
                Configs::dataManager->profilesRepo->Save(ent);
                MW_show_log(tr("[Auto selector] Running the best %1 of %2 ranked profiles.")
                                .arg(builtIDs.size())
                                .arg(selector->pool.size()));
            }
        }

        Configs::dataManager->settingsRepo->UpdateStartedId(ent->id);
        // `ip_out` is persisted from the last probe. Do not show that stale
        // result as a live connection until the new profile instance proves
        // its actual egress address.
        ent->ip_out.clear();
        ent->test_country.clear();
        running = ent;
        if (Configs::dataManager->settingsRepo->spmode_system_proxy) set_system_proxy(true);

        runOnUiThread([=, this] {
            refresh_status();
            refresh_proxy_list({ent->id});
            // This test builds a one-profile test core, so its result is the
            // profile's real egress IP even when the application's split route
            // would send a normal HTTP lookup directly.
            testRunner->runIpTests({ent->id});
            // Reveals the Tools entry and seeds the data-view panel before the
            // first poll lands, so a selector never starts up invisibly.
            refresh_auto_selector_view();

            auto resp = NetworkRequestHelper::HttpGet("http://ip-api.com/json/", false, true);
            if (resp.error.isEmpty()) {
                QJsonDocument doc = QJsonDocument::fromJson(resp.data);
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();
                    QString city = obj["city"].toString();
                    QString countryName = obj["country"].toString();
                    QString countryCode = obj["countryCode"].toString();
                    if (running) running->runningCountryInfo = QString("%1 %2, %3").arg(CountryCodeToFlag(countryCode), countryName, city);
                    refresh_status();
                }
            }
        });

        return true;
    };

    if (!mu_starting.tryLock()) {
        MessageBoxWarning(software_name, tr("Another profile is starting..."));
        return;
    }
    if (!mu_stopping.tryLock()) {
        MessageBoxWarning(software_name, tr("Another profile is stopping..."));
        mu_starting.unlock();
        return;
    }
    mu_stopping.unlock();

    // check core state
    if (!Configs::dataManager->settingsRepo->core_running) {
        runOnThread(
            [=, this] {
                MW_show_log(tr("Try to start the config, but the core has not listened to the RPC port, so restart it..."));
                core_process->start_profile_when_core_is_up = ent->id;
                core_process->Restart();
            },
            DS_cores);
        mu_starting.unlock();
        return; // let CoreProcess call profile_start when core is up
    }

    // timeout message
    const auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=,this] { MW_dialog_message(MwMessage::RestartProgram, {}); });
    const auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 10000);

    // Show the "Connecting" state until the start resolves below.
    runOnUiThread([this] {
        m_profileConnecting = true;
        refresh_startstop_button();
    });

    runOnNewThread([=, this] {
        // stop current running
        if (running != nullptr) {
            profile_stop(false, false, true);
            mu_stopping.lock();
            mu_stopping.unlock();
        }
        // do start
        MW_show_log(">>>>>>>> " + tr("Starting profile %1").arg(ent->outbound->DisplayTypeAndName()));
        if (!profile_start_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to start profile %1").arg(ent->outbound->DisplayTypeAndName()));
        }
        mu_starting.unlock();
        // cancel timeout
        runOnUiThread([=, this] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
            // Start has resolved (success or failure); leave the Connecting state.
            m_profileConnecting = false;
            refresh_startstop_button();
        });
    });
}

void MainWindow::profile_stop(bool crash, bool block, bool manual) {
    if (running == nullptr) {
        return;
    }
    const auto id = running->id;

    auto profile_stop_stage2 = [=,this] {
        if (testRunner->isTestingCurrent()) {
            bool ok;
            defaultClient->StopTests(&ok);
            if (!ok) MW_show_log("Failed to stop profile tests!");
        }
        if (!crash) {
            bool rpcOK;
            const QString error = defaultClient->Stop(&rpcOK);
            if (rpcOK && !error.isEmpty()) {
                runOnUiThread([=,this] { MessageBoxWarning(tr("Stop return error"), error); });
                return false;
            } else if (!rpcOK) {
                return false;
            }
        }
        if (Configs::dataManager->settingsRepo->spmode_system_proxy) set_system_proxy(false);
        return true;
    };

    if (!mu_stopping.tryLock()) {
        return;
    }

    UpdateConnectionListWithRecreate({});

    // Show a "Disconnecting" spinner immediately; the stop itself can lag.
    runOnUiThread([this] {
        m_profileDisconnecting = true;
        refresh_startstop_button();
    });

    Stats::autoSelectorMonitor->Clear();
    runOnUiThread([this] { refresh_auto_selector_view(); });

    runOnNewThread([=, this] {
        Stats::trafficLooper->loop_enabled = false;
        Stats::connection_lister->suspend = true;
        Stats::trafficLooper->loop_mutex.lock();
        Stats::trafficLooper->UpdateAll();
        Stats::trafficLooper->loop_mutex.unlock();
        // Flush the final per-profile totals (only persisted every few seconds
        // during the session) and the partial minute bucket before going down.
        Stats::trafficLooper->PersistTraffic();
        Stats::trafficStatsManager->Flush();

        // Null until the blocking hop assigns them: runOnUiThread is a no-op before qApp
        // exists, and the teardown must not chase an uninitialized pointer.
        QMessageBox* restartMsgbox = nullptr;
        MessageBoxTimer* restartMsgboxTimer = nullptr;
        runOnUiThread([=, this, &restartMsgbox, &restartMsgboxTimer] {
            restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                             QMessageBox::Yes | QMessageBox::No, this);
            connect(restartMsgbox, &QMessageBox::accepted, this, [=, this] { MW_dialog_message(MwMessage::RestartProgram, {}); });
            restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);
        }, true);

        // do stop. Snapshot the profile: `running` is cleared below and can also
        // be reassigned by a start racing this teardown.
        const auto stopping = running;
        if (stopping != nullptr) {
            MW_show_log(">>>>>>>> " + tr("Stopping profile %1").arg(stopping->outbound->DisplayTypeAndName()));
        }
        if (!profile_stop_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to stop, please restart the program."));
        }

        if (manual) Configs::dataManager->settingsRepo->UpdateStartedId(Configs::NoProfileId);
        running = nullptr;

        runOnUiThread([=, this, &restartMsgboxTimer, &restartMsgbox] {
            if (restartMsgboxTimer != nullptr) {
                restartMsgboxTimer->cancel();
                restartMsgboxTimer->deleteLater();
            }
            if (restartMsgbox != nullptr) restartMsgbox->deleteLater();

            m_profileDisconnecting = false;
            refresh_status();
            refresh_proxy_list({id});

            mu_stopping.unlock();
        }, true);
    }, block);
}
