#include "include/ui/mainwindow.h"

#include "include/stats/traffic/TrafficLooper.hpp"
#include "include/api/RPC.h"
#include "include/ui/utils//MessageBoxTimer.h"
#include "3rdparty/qv2ray/v2/proxy/QvProxyConfigurator.hpp"

#include <QInputDialog>
#include <QPushButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSemaphore>

#include <algorithm>
#include <cmath>
#include <QElapsedTimer>

#include "include/configs/generate.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/CountryHelper.hpp"

#include "include/sys/Process.hpp"

// rpc

using namespace API;

namespace {
double ParseSpeedToMbps(QString speed) {
    speed = speed.trimmed();
    if (speed.isEmpty()) return 0.0;
    QRegularExpression re(R"(([-+]?\d*\.?\d+)\s*([kKmMgG])?bps)");
    auto m = re.match(speed);
    if (!m.hasMatch()) return 0.0;

    const double v = m.captured(1).toDouble();
    const QString u = m.captured(2).toLower();
    if (u == "g") return v * 1000.0;
    if (u == "m") return v;
    if (u == "k") return v / 1000.0;
    return v / 1000000.0;
}

/**
 * Calculates "Site Score" (0-100) representing perceived web browsing performance.
 * Logic:
 * - This model simulates typical browsing (opening new sockets, then transferring data).
 * - Rx speed remains the primary factor.
 * - Latency penalty is piecewise: mild below 120ms, stronger after 250ms.
 */
int CalcSiteScore(int connectMs, double rxMbps) {
    if (connectMs <= 0 || rxMbps <= 0.0) return 0;
    auto clamp01 = [](double v) { return std::max(0.0, std::min(100.0, v)); };
    const double ms = static_cast<double>(connectMs);

    double penalty = 0.0;
    if (ms <= 120.0) {
        penalty = ms * 0.08;
    } else if (ms <= 250.0) {
        penalty = 120.0 * 0.08 + (ms - 120.0) * 0.18;
    } else {
        penalty = 120.0 * 0.08 + 130.0 * 0.18 + (ms - 250.0) * 0.38;
    }

    const double connectScore = clamp01(100.0 - penalty);
    const double rxScore = clamp01(rxMbps * 0.92);
    return static_cast<int>(std::round(connectScore * 0.25 + rxScore * 0.75));
}

int ResolveEntID(const QMap<QString, int>& tag2entID, int fallbackEntID, const std::string& outboundTag) {
    if (tag2entID.empty()) return fallbackEntID;
    return tag2entID.value(QString::fromStdString(outboundTag), -1);
}
}

void MainWindow::setup_rpc() {
    // Setup Connection
    defaultClient = new Client(
        [=](const QString &errStr) {
            MW_show_log("[Error] Core: " + errStr);
        },
        "127.0.0.1:" + Int2String(Configs::dataManager->settingsRepo->core_port));

    // Looper
    runOnNewThread([=] { Stats::trafficLooper->Loop(); });
    runOnNewThread([=] {Stats::connection_lister->Loop(); });
}

void MainWindow::runURLTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID,
                            const QString& testUrl, bool saveConnectTime, int timeoutMsOverride) {
    if (stopSpeedtest.load()) {
        MW_show_log(tr("Profile test aborted"));
        return;
    }

    libcore::TestReq req;
    for (const auto &item: outboundTags) {
        req.outbound_tags.push_back(item.toStdString());
    }
    req.config = config.toStdString();
    req.url = (testUrl.isEmpty() ? Configs::dataManager->settingsRepo->simple_dl_url : testUrl).toStdString();
    req.use_default_outbound = useDefault;
    req.max_concurrency = Configs::dataManager->settingsRepo->test_concurrent;
    req.test_timeout_ms = timeoutMsOverride > 0 ? timeoutMsOverride : Configs::dataManager->settingsRepo->url_test_timeout_ms;
    req.xray_config = xrayConfig.toStdString();
    req.need_xray = !xrayConfig.isEmpty();

    auto done = new QMutex;
    done->lock();
    runOnNewThread([=,this]
    {
        bool ok;
        while (true)
        {
            QThread::msleep(200);
            if (done->try_lock()) break;
            auto resp = defaultClient->QueryURLTest(&ok);
            if (!ok || resp.results.empty())
            {
                continue;
            }

            bool needRefresh = false;
            QList<int> profileIDs;
            for (const auto& res : resp.results)
            {
                int entid = ResolveEntID(tag2entID, -1, res.outbound_tag.value());
                if (entid == -1) {
                    continue;
                }
                profileIDs << entid;
                auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (ent == nullptr) {
                    continue;
                }

                runOnUiThread([=, this] {
                    showSpeedtestData = true;
                    currentSptProfileName = ent->outbound->name;
                    currentTestStatusText = saveConnectTime ? tr("Connection Test") : tr("Latency Test");
                    currentTestResult = {};
                    UpdateDataView(true);
                });

                if (res.error.value().empty()) {
                    if (saveConnectTime) ent->connect_time_ms = res.latency_ms.value();
                    else ent->latency = res.latency_ms.value();
                } else {
                    if (QString::fromStdString(res.error.value()).contains("test aborted") ||
                        QString::fromStdString(res.error.value()).contains("context canceled")) {
                        if (saveConnectTime) ent->connect_time_ms = 0;
                        else ent->latency = 0;
                    }
                    else {
                        if (saveConnectTime) ent->connect_time_ms = -1;
                        else ent->latency = -1;
                        MW_show_log(tr("[%1] test error: %2").arg(ent->outbound->DisplayTypeAndName(), QString::fromStdString(res.error.value())));
                    }
                }
                Configs::dataManager->profilesRepo->Save(ent);
                needRefresh = true;
            }
            if (needRefresh)
            {
                runOnUiThread([=,this]{
                    refresh_proxy_list(profileIDs);
                });
            }
        }
        done->unlock();
        delete done;
        runOnUiThread([=, this] {
            QTimer::singleShot(1000, this, [this] {
                showSpeedtestData = false;
                UpdateDataView();
            });
        });
    });
    bool rpcOK;
    auto result = defaultClient->Test(&rpcOK, req);
    done->unlock();
    //
    if (!rpcOK || result.results.empty()) {
        QList<int> idsToFail;
        if (entID >= 0) {
            idsToFail << entID;
        } else {
            for (auto it = tag2entID.constBegin(); it != tag2entID.constEnd(); ++it) {
                idsToFail << it.value();
            }
        }
        for (int id : idsToFail) {
            auto ent = Configs::dataManager->profilesRepo->GetProfile(id);
            if (!ent) continue;
            ent->dl_speed = "Error";
            ent->dl_speed_mbps = 0.0;
            if (ent->ul_speed.isEmpty()) ent->ul_speed = "N/A";
            if (ent->ul_speed_mbps < 0.0) ent->ul_speed_mbps = 0.0;
            if (ent->connect_time_ms <= 0) ent->connect_time_ms = -1;
            if (ent->latency <= 0) ent->latency = -1;
            ent->site_score = -2;
            Configs::dataManager->profilesRepo->Save(ent);
            int profileId = ent->id;
            runOnUiThread([=, this]() {
                refresh_proxy_list({profileId});
            });
            MW_show_log(tr("[%1] speed test error: rpc failed or empty result").arg(ent->outbound->DisplayTypeAndName()));
        }
        return;
    }

    for (const auto &res: result.results) {
        entID = ResolveEntID(tag2entID, entID, res.outbound_tag.value());
        if (entID == -1) {
            MW_show_log(tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }

        auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
        if (ent == nullptr) {
            MW_show_log(tr("Profile manager data is corrupted, try again."));
            continue;
        }

        if (res.error.value().empty()) {
            if (saveConnectTime) ent->connect_time_ms = res.latency_ms.value();
            else ent->latency = res.latency_ms.value();
        } else {
            if (QString::fromStdString(res.error.value()).contains("test aborted") ||
                QString::fromStdString(res.error.value()).contains("context canceled")) {
                if (saveConnectTime) ent->connect_time_ms = 0;
                else ent->latency = 0;
            }
            else {
                if (saveConnectTime) ent->connect_time_ms = -1;
                else ent->latency = -1;
                MW_show_log(tr("[%1] test error: %2").arg(ent->outbound->DisplayTypeAndName(), QString::fromStdString(res.error.value())));
            }
        }
        Configs::dataManager->profilesRepo->Save(ent);
    }
}

void MainWindow::runIPTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID) {
    if (stopSpeedtest.load()) {
        MW_show_log(tr("Profile test aborted"));
        return;
    }

    libcore::IPTestRequest req;
    for (const auto &item: outboundTags) {
        req.outbound_tags.push_back(item.toStdString());
    }
    req.config = config.toStdString();
    req.use_default_outbound = useDefault;
    req.max_concurrency = Configs::dataManager->settingsRepo->test_concurrent;
    req.test_timeout_ms = Configs::dataManager->settingsRepo->url_test_timeout_ms;
    req.xray_config = xrayConfig.toStdString();
    req.need_xray = !xrayConfig.isEmpty();

    auto done = new QMutex;
    done->lock();
    runOnNewThread([=,this]
    {
        bool ok;
        while (true)
        {
            QThread::msleep(200);
            if (done->try_lock()) break;
            auto resp = defaultClient->QueryIPTest(&ok);
            if (!ok || resp.results.empty())
            {
                continue;
            }

            bool needRefresh = false;
            QList<int> profileIDs;
            for (const auto& res : resp.results)
            {
                int entid = ResolveEntID(tag2entID, -1, res.outbound_tag.value());
                if (entid == -1) {
                    continue;
                }
                profileIDs << entid;
                auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (ent == nullptr) {
                    continue;
                }

                runOnUiThread([=, this] {
                    showSpeedtestData = true;
                    currentSptProfileName = ent->outbound->name;
                    currentTestStatusText = tr("IP Test");
                    currentTestResult = {};
                    UpdateDataView();
                });

                if (res.error.value().empty()) {
                    ent->ip_out = QString::fromStdString(res.ip.value());
                } else {
                    if (!QString::fromStdString(res.error.value()).contains("test aborted") &&
                        !QString::fromStdString(res.error.value()).contains("context canceled")) {
                        MW_show_log(tr("[%1] IP test error: %2").arg(ent->outbound->DisplayTypeAndName(), QString::fromStdString(res.error.value())));
                    }
                    ent->ip_out.clear();
                }
                Configs::dataManager->profilesRepo->Save(ent);
                needRefresh = true;
            }
            if (needRefresh)
            {
                runOnUiThread([=,this]{
                    refresh_proxy_list(profileIDs);
                });
            }
        }
        done->unlock();
        delete done;
        runOnUiThread([=, this] {
            QTimer::singleShot(1000, this, [this] {
                showSpeedtestData = false;
                UpdateDataView();
            });
        });
    });
    bool rpcOK;
    auto result = defaultClient->IPTest(&rpcOK, req);
    done->unlock();
    //
    if (!rpcOK || result.results.empty()) return;

    for (const auto &res: result.results) {
        entID = ResolveEntID(tag2entID, entID, res.outbound_tag.value());
        if (entID == -1) {
            MW_show_log(tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }

        auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
        if (ent == nullptr) {
            MW_show_log(tr("Profile manager data is corrupted, try again."));
            continue;
        }

        if (res.error.value().empty()) {
            ent->ip_out = QString::fromStdString(res.ip.value());
        } else {
            if (!QString::fromStdString(res.error.value()).contains("test aborted") &&
                !QString::fromStdString(res.error.value()).contains("context canceled")) {
                MW_show_log(tr("[%1] IP test error: %2").arg(ent->outbound->DisplayTypeAndName(), QString::fromStdString(res.error.value())));
            }
            ent->ip_out.clear();
        }
        Configs::dataManager->profilesRepo->Save(ent);
    }
}

void MainWindow::urltest_current_group(const QList<int>& profileIDs, bool connectionTimeTest) {
    if (profileIDs.isEmpty()) {
        return;
    }
    if (speedtestRunning.exchange(true)) {
        MW_show_log(tr("A test is already running; ignoring duplicate start request."));
        return;
    }

    runOnNewThread([this, profileIDs, connectionTimeTest]() {
        Q_UNUSED(connectionTimeTest);
        stopSpeedtest.store(false);
        runConnectionTimeTestsForProfiles(profileIDs, true);
        speedtestRunning.store(false);
        MW_show_log(tr("Connection-time test finished!"));
    });
}

QList<int> MainWindow::getOrderedSpeedtestProfileIDs(const QList<int>& profileIDs, SpeedtestStartMode startMode) const {
    if (startMode == SpeedtestStartMode::AsIs || profileIDs.size() < 2) {
        return profileIDs;
    }

    QList<int> ordered = profileIDs;
    if (startMode == SpeedtestStartMode::ByConnectionTime) {
        bool hasPositive = false;
        for (int id : ordered) {
            auto p = Configs::dataManager->profilesRepo->GetProfile(id);
            if (p && p->connect_time_ms > 0) {
                hasPositive = true;
                break;
            }
        }
        if (!hasPositive) return profileIDs;

        std::stable_sort(ordered.begin(), ordered.end(), [](int lhs, int rhs) {
            auto l = Configs::dataManager->profilesRepo->GetProfile(lhs);
            auto r = Configs::dataManager->profilesRepo->GetProfile(rhs);
            const bool lHas = (l != nullptr && l->connect_time_ms > 0);
            const bool rHas = (r != nullptr && r->connect_time_ms > 0);
            if (lHas != rHas) return lHas > rHas;
            if (!lHas) return false;
            return l->connect_time_ms < r->connect_time_ms;
        });
        return ordered;
    }

    bool hasPositive = false;
    for (int id : ordered) {
        auto p = Configs::dataManager->profilesRepo->GetProfile(id);
        if (p && p->site_score > 0) {
            hasPositive = true;
            break;
        }
    }
    if (!hasPositive) return profileIDs;

    std::stable_sort(ordered.begin(), ordered.end(), [](int lhs, int rhs) {
        auto l = Configs::dataManager->profilesRepo->GetProfile(lhs);
        auto r = Configs::dataManager->profilesRepo->GetProfile(rhs);
        const bool lHas = (l != nullptr && l->site_score > 0);
        const bool rHas = (r != nullptr && r->site_score > 0);
        if (lHas != rHas) return lHas > rHas;
        if (!lHas) return false;
        return l->site_score > r->site_score;
    });
    return ordered;
}

bool MainWindow::runConnectionTimeTestsForProfiles(const QList<int>& profileIDs, bool clearUnavailableAfter) {
    if (profileIDs.isEmpty()) return true;
    const int chunkSize = qMax(1, parallelCoreCallPool->maxThreadCount());
    const bool fallShortEnabled = Configs::dataManager->settingsRepo->speed_test_fall_short;

    if (fallShortEnabled) {
        qint64 minConnectionTimeMs = -1;
        bool completed = true;

        for (int entID : profileIDs) {
            if (stopSpeedtest.load()) {
                completed = false;
                break;
            }

            auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
            if (!ent) continue;

            auto buildObject = Configs::BuildTestConfig({ent});
            if (!buildObject->error.isEmpty()) {
                MW_show_log(tr("Failed to build test config for profile %1: ").arg(ent->outbound->DisplayTypeAndName()) + buildObject->error);
                continue;
            }

            int timeoutMs = Configs::dataManager->settingsRepo->url_test_timeout_ms;
            if (minConnectionTimeMs > 0) {
                const qint64 threshold = std::max<qint64>(1, minConnectionTimeMs * 3);
                timeoutMs = std::min<qint64>(timeoutMs, threshold);
            }

            if (buildObject->fullConfigs.contains(entID)) {
                runURLTest(buildObject->fullConfigs[entID], "", true, {}, {}, entID,
                           Configs::dataManager->settingsRepo->simple_dl_url, true, timeoutMs);
            } else {
                auto xrayConf = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                runURLTest(QJsonObject2QString(buildObject->coreConfig, false), xrayConf, false,
                           buildObject->outboundTags, buildObject->tag2entID, -1,
                           Configs::dataManager->settingsRepo->simple_dl_url, true, timeoutMs);
            }

            auto updated = Configs::dataManager->profilesRepo->GetProfile(entID);
            if (!updated || updated->connect_time_ms <= 0) {
                MW_show_log(tr("[%1] connection test skipped by fall-short threshold (%2 ms)")
                                .arg(ent->outbound->DisplayTypeAndName())
                                .arg(timeoutMs));
                continue;
            }

            if (minConnectionTimeMs <= 0 || updated->connect_time_ms < minConnectionTimeMs) {
                minConnectionTimeMs = updated->connect_time_ms;
                MW_show_log(tr("Connection test baseline updated: %1 ms").arg(minConnectionTimeMs));
            }
        }

        if (completed && clearUnavailableAfter) {
            auto first = Configs::dataManager->profilesRepo->GetProfile(profileIDs.first());
            auto currentGroup = first ? Configs::dataManager->groupsRepo->GetGroup(first->gid) : nullptr;
            if (currentGroup && currentGroup->auto_clear_unavailable) {
                MW_show_log("Connection-time test finished, clearing unavailable profiles...");
                runOnUiThread([=, this] { clearUnavailableProfiles(false, profileIDs); });
            }
        }
        return completed;
    }

    auto runSlice = [=, this](const QList<std::shared_ptr<Configs::Profile>>& profileSlice, const QList<int>& ids) {
        auto buildObject = Configs::BuildTestConfig(profileSlice);
        if (!buildObject->error.isEmpty()) {
            MW_show_log(tr("Failed to build test config for batch: ") + buildObject->error);
            return;
        }

        const int testCount = buildObject->fullConfigs.size() + (!buildObject->outboundTags.empty());
        if (testCount == 0) return;

        QSemaphore doneSem(0);
        for (const auto &entID: buildObject->fullConfigs.keys()) {
            auto configStr = buildObject->fullConfigs[entID];
            auto func = [this, &doneSem, configStr, entID]() {
                runURLTest(configStr, "", true, {}, {}, entID,
                           Configs::dataManager->settingsRepo->simple_dl_url,
                           true);
                doneSem.release();
            };
            parallelCoreCallPool->start(func);
        }

        if (!buildObject->outboundTags.empty()) {
            auto func = [this, &buildObject, &doneSem]() {
                auto xrayConf = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                runURLTest(QJsonObject2QString(buildObject->coreConfig, false), xrayConf, false, buildObject->outboundTags,
                           buildObject->tag2entID, -1, Configs::dataManager->settingsRepo->simple_dl_url, true);
                doneSem.release();
            };
            parallelCoreCallPool->start(func);
        }

        for (int i = 0; i < testCount; ++i) doneSem.acquire();
        MW_show_log("Connection-time test for batch done.");
        runOnUiThread([=, this] { refresh_proxy_list(ids); });
    };

    std::shared_ptr<Configs::Group> currentGroup;
    bool completed = true;
    for (int i = 0; i < profileIDs.length(); i += chunkSize) {
        if (stopSpeedtest.load()) {
            completed = false;
            break;
        }
        auto profileIDsSlice = profileIDs.mid(i, chunkSize);
        auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDsSlice);
        if (!currentGroup && !profiles.isEmpty()) {
            currentGroup = Configs::dataManager->groupsRepo->GetGroup(profiles[0]->gid);
        }
        runSlice(profiles, profileIDsSlice);
    }

    if (completed && clearUnavailableAfter && currentGroup && currentGroup->auto_clear_unavailable) {
        MW_show_log("Connection-time test finished, clearing unavailable profiles...");
        runOnUiThread([=, this] { clearUnavailableProfiles(false, profileIDs); });
    }
    return completed;
}

bool MainWindow::runSpeedtestConnectionPretestIfNeeded(const QList<int>& profileIDs, SpeedtestStartMode startMode) {
    if (startMode != SpeedtestStartMode::ByConnectionTime) {
        return true;
    }
    // Reuse the exact standalone connection-time test pipeline.
    return runConnectionTimeTestsForProfiles(profileIDs, false);
}

void MainWindow::stopTests() {
    stopSpeedtest.store(true);
    speedtestRunning.store(false);
    bool ok;
    defaultClient->StopTests(&ok);

    runOnUiThread([=, this] {
        ui->pushButton_cancel_speedtest->setEnabled(false);
        showSpeedtestData = false;
        currentTestStatusText.clear();
        currentSptProfileName.clear();
        UpdateDataView(true);
    });

    if (!ok) {
        MW_show_log(tr("Failed to stop tests"));
    }
}

void MainWindow::url_test_current() {
    last_test_time = QDateTime::currentSecsSinceEpoch();
    ui->label_running->setText(tr("Testing"));

    runOnNewThread([=,this] {
        libcore::TestReq req;
        req.test_current = true;
        req.url = Configs::dataManager->settingsRepo->simple_dl_url.toStdString();

        bool rpcOK;
        auto result = defaultClient->Test(&rpcOK, req);
        if (!rpcOK || result.results.empty()) return;

        auto connectTime = result.results[0].latency_ms.value();
        last_test_time = QDateTime::currentSecsSinceEpoch();

        runOnUiThread([=,this] {
            if (!result.results[0].error.value().empty()) {
                MW_show_log(QString("Connection-time test error: %1").arg(QString::fromStdString(result.results[0].error.value())));
            }
            if (connectTime <= 0) {
                ui->label_running->setText(tr("Connection Time") + ": " + tr("Unavailable"));
            } else if (connectTime > 0) {
                ui->label_running->setText(tr("Connection Time") + ": " + QString("%1 ms").arg(connectTime));
            }
        });
    });
}

void MainWindow::iptest_current_group(const QList<int>& profileIDs) {
    if (profileIDs.isEmpty()) {
        return;
    }
    if (speedtestRunning.exchange(true)) {
        MW_show_log(tr("A test is already running; ignoring duplicate start request."));
        return;
    }

    runOnNewThread([this, profileIDs]() {
        stopSpeedtest.store(false);
        const int chunkSize = qMax(1, parallelCoreCallPool->maxThreadCount());
        auto ipTestFunc = [=, this](const QList<std::shared_ptr<Configs::Profile>>& profileSlice, const QList<int>& ids) {
            auto buildObject = Configs::BuildTestConfig(profileSlice);
            if (!buildObject->error.isEmpty()) {
                MW_show_log(tr("Failed to build test config for batch: ") + buildObject->error);
                return;
            }

            auto testCount = buildObject->fullConfigs.size() + (!buildObject->outboundTags.empty());
            if (testCount == 0) {
                return;
            }

            QSemaphore doneSem(0);
            for (const auto &entID: buildObject->fullConfigs.keys()) {
                auto configStr = buildObject->fullConfigs[entID];
                auto func = [this, &doneSem, configStr, entID]() {
                    runIPTest(configStr, "", true, {}, {}, entID);
                    doneSem.release();
                };
                parallelCoreCallPool->start(func);
            }

            if (!buildObject->outboundTags.empty()) {
                auto func = [this, &buildObject, &doneSem]() {
                    auto xrayConf = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                    runIPTest(QJsonObject2QString(buildObject->coreConfig, false), xrayConf, false, buildObject->outboundTags, buildObject->tag2entID);
                    doneSem.release();
                };
                parallelCoreCallPool->start(func);
            }

            for (int i = 0; i < testCount; ++i) {
                doneSem.acquire();
            }
            MW_show_log("IP test for batch done.");
            runOnUiThread([=,this]{
                refresh_proxy_list(ids);
            });
        };
        for (int i = 0; i < profileIDs.length(); i += chunkSize) {
            if (stopSpeedtest.load()) break;
            auto profileIDsSlice = profileIDs.mid(i, chunkSize);
            auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDsSlice);
            ipTestFunc(profiles, profileIDsSlice);
        }
        speedtestRunning.store(false);
        MW_show_log(tr("IP test finished!"));
    });
}

void MainWindow::speedtest_current_group(const QList<int>& profileIDs, SpeedtestConnectMode connectMode, SpeedtestStartMode startMode)
{
    if (ui->actionSpeed_test_fall_short != nullptr && ui->actionSpeed_test_fall_short->isChecked()) {
        speedtest_current_group_fall_short(profileIDs, connectMode, startMode);
        return;
    }

    if (profileIDs.isEmpty()) {
        return;
    }

    if (speedtestRunning.exchange(true)) {
        MW_show_log(tr("A test is already running; ignoring duplicate start request."));
        return;
    }

    int profileToRestore = deferred_profile_start_after_speedtest.exchange(-1919);
    if (Configs::dataManager->settingsRepo->started_id >= 0) {
        profileToRestore = Configs::dataManager->settingsRepo->started_id;
    }
    if (running != nullptr) {
        profileToRestore = running->id;
    }

    runOnUiThread([=, this] {
        ui->pushButton_cancel_speedtest->setVisible(true);
        ui->pushButton_cancel_speedtest->setEnabled(true);
    });

    runOnNewThread([this, profileIDs, profileToRestore, connectMode, startMode]() {
        bool completedFully = true;
        const int chunkSize = qMax(1, parallelCoreCallPool->maxThreadCount());
        if (Configs::dataManager->settingsRepo->started_id >= 0) {
            // Use the exact same stop path as Ctrl+S / Stop action.
            runOnUiThread([=, this] {
                ui->menu_stop->trigger();
            }, true);

            // Wait until profile is fully stopped before running speedtest.
            int waitTicks = 0;
            while (Configs::dataManager->settingsRepo->started_id >= 0 && waitTicks++ < 200) {
                QThread::msleep(50);
            }

            if (Configs::dataManager->settingsRepo->started_id >= 0) {
                speedtestRunning.store(false);
                runOnUiThread([=, this] {
                    ui->pushButton_cancel_speedtest->setVisible(false);
                    ui->pushButton_cancel_speedtest->setEnabled(false);
                    MW_show_log(tr("Failed to stop active profile before speedtest; speedtest cancelled."));
                });
                return;
            }
        }

        stopSpeedtest.store(false);
        completedFully = runSpeedtestConnectionPretestIfNeeded(profileIDs, startMode);
        QList<int> orderedIds = getOrderedSpeedtestProfileIDs(profileIDs, startMode);

        auto speedTestFunc = [=, &completedFully, this](const QList<std::shared_ptr<Configs::Profile>>& profileSlice) {
            if (stopSpeedtest.load()) {
                completedFully = false;
                return;
            }

            auto buildObject = Configs::BuildTestConfig(profileSlice);
            if (!buildObject->error.isEmpty()) {
                MW_show_log(tr("Failed to build batch test config: ") + buildObject->error);
                return;
            }

            // 1) 2MB simple download throughput probe.
            for (const auto &entID: buildObject->fullConfigs.keys()) {
                if (stopSpeedtest.load()) {
                    completedFully = false;
                    return;
                }
                auto configStr = buildObject->fullConfigs[entID];
                runSpeedTest(configStr, "", true, {}, {}, entID);
            }
            if (stopSpeedtest.load()) {
                completedFully = false;
                return;
            }
            if (!buildObject->outboundTags.empty()) {
                auto xrayConf = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                runSpeedTest(QJsonObject2QString(buildObject->coreConfig, false), xrayConf, false, buildObject->outboundTags, buildObject->tag2entID);
            }
        };

        for (int i=0;i<orderedIds.length();i+=chunkSize) {
            if (stopSpeedtest.load()) {
                completedFully = false;
                break;
            }
            auto profileIDsSlice = orderedIds.mid(i, chunkSize);
            auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDsSlice);
            speedTestFunc(profiles);
            if (!completedFully) {
                break;
            }
        }

        const bool canAutoConnect = completedFully && !stopSpeedtest.load();
        speedtestRunning.store(false);
        runOnUiThread([=,this]{
            // Finalize with a full model refresh to avoid stale row state after speedtest completes.
            refresh_proxy_list({}, true);
            ui->pushButton_cancel_speedtest->setVisible(false);
            ui->pushButton_cancel_speedtest->setEnabled(false);
            MW_show_log(canAutoConnect ? tr("Speedtest finished!") : tr("Speedtest interrupted."));

            if (connectMode != SpeedtestConnectMode::None && !canAutoConnect) {
                MW_show_log(tr("Auto connect skipped because speedtest did not finish completely."));
                return;
            }

            int bestId = -1;
            if (connectMode == SpeedtestConnectMode::BestConnectionTime) {
                int bestConn = std::numeric_limits<int>::max();
                for (int id : profileIDs) {
                    auto p = Configs::dataManager->profilesRepo->GetProfile(id);
                    if (!p || p->connect_time_ms <= 0) continue;
                    if (p->connect_time_ms < bestConn) {
                        bestConn = p->connect_time_ms;
                        bestId = id;
                    }
                }
            } else if (connectMode == SpeedtestConnectMode::BestSiteScore) {
                int bestScore = -1;
                for (int id : profileIDs) {
                    auto p = Configs::dataManager->profilesRepo->GetProfile(id);
                    if (!p || p->site_score <= 0) continue;
                    if (p->site_score > bestScore) {
                        bestScore = p->site_score;
                        bestId = id;
                    }
                }
            }

            if (bestId >= 0) {
                profile_start(bestId);
            } else if (connectMode == SpeedtestConnectMode::None && profileToRestore >= 0) {
                profile_start(profileToRestore);
            }
        });
    });
}

void MainWindow::speedtest_current_group_fall_short(const QList<int>& profileIDs, SpeedtestConnectMode connectMode, SpeedtestStartMode startMode)
{
    if (profileIDs.isEmpty()) {
        MW_show_log(tr("No selected profiles for fall-short speedtest."));
        return;
    }

    if (speedtestRunning.exchange(true)) {
        MW_show_log(tr("A test is already running; ignoring duplicate start request."));
        return;
    }

    int profileToRestore = deferred_profile_start_after_speedtest.exchange(-1919);
    if (Configs::dataManager->settingsRepo->started_id >= 0) {
        profileToRestore = Configs::dataManager->settingsRepo->started_id;
    }
    if (running != nullptr) {
        profileToRestore = running->id;
    }

    runOnUiThread([=, this] {
        ui->pushButton_cancel_speedtest->setVisible(true);
        ui->pushButton_cancel_speedtest->setEnabled(true);
        showSpeedtestData = true;
        currentTestStatusText = tr("Speedtest (Fall-short)");
    });

    runOnNewThread([this, profileIDs, profileToRestore, connectMode, startMode]() {
        bool completedFully = true;
        if (Configs::dataManager->settingsRepo->started_id >= 0) {
            runOnUiThread([=, this] {
                ui->menu_stop->trigger();
            }, true);

            int waitTicks = 0;
            while (Configs::dataManager->settingsRepo->started_id >= 0 && waitTicks++ < 200) {
                QThread::msleep(50);
            }

            if (Configs::dataManager->settingsRepo->started_id >= 0) {
                speedtestRunning.store(false);
                runOnUiThread([=, this] {
                    ui->pushButton_cancel_speedtest->setVisible(false);
                    ui->pushButton_cancel_speedtest->setEnabled(false);
                    MW_show_log(tr("Failed to stop active profile before speedtest; speedtest cancelled."));
                });
                return;
            }
        }

        stopSpeedtest.store(false);
        completedFully = runSpeedtestConnectionPretestIfNeeded(profileIDs, startMode);
        QList<int> orderedIds = getOrderedSpeedtestProfileIDs(profileIDs, startMode);

        qint64 minFinishedMs = -1;
        qint64 minConnectionTimeMs = -1;
        const int defaultTimeoutMs = Configs::dataManager->settingsRepo->speed_test_timeout_ms;

        for (int id : orderedIds) {
            auto p = Configs::dataManager->profilesRepo->GetProfile(id);
            if (!p || p->connect_time_ms <= 0) continue;
            if (minConnectionTimeMs <= 0 || p->connect_time_ms < minConnectionTimeMs) {
                minConnectionTimeMs = p->connect_time_ms;
            }
        }

        for (int entID : orderedIds) {
            if (stopSpeedtest.load()) {
                completedFully = false;
                break;
            }

            auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
            if (!ent) continue;

            auto buildObject = Configs::BuildTestConfig({ent});
            if (!buildObject->error.isEmpty()) {
                MW_show_log(tr("Failed to build test config for profile %1: ").arg(ent->outbound->DisplayTypeAndName()) + buildObject->error);
                continue;
            }

            int timeoutMs = defaultTimeoutMs;
            if (minFinishedMs > 0) {
                const qint64 threshold = std::max<qint64>(1, minFinishedMs * 2);
                timeoutMs = std::min<qint64>(defaultTimeoutMs, threshold);
            }
            if (minConnectionTimeMs > 0) {
                const qint64 connectionThreshold = std::max<qint64>(1, minConnectionTimeMs * 3);
                timeoutMs = std::min<qint64>(timeoutMs, connectionThreshold);
            }

            qint64 elapsedMs = 0;
            bool success = false;
            bool skipped = false;

            if (buildObject->fullConfigs.contains(entID)) {
                runSpeedTestFallShort(buildObject->fullConfigs[entID], "", true, {}, {}, entID, timeoutMs,
                                      &elapsedMs, &success, &skipped);
            } else {
                auto xrayConf = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                runSpeedTestFallShort(QJsonObject2QString(buildObject->coreConfig, false), xrayConf, false,
                                      buildObject->outboundTags, buildObject->tag2entID, -1, timeoutMs,
                                      &elapsedMs, &success, &skipped);
            }

            Q_UNUSED(skipped);
            if (success && elapsedMs > 0) {
                if (minFinishedMs <= 0 || elapsedMs < minFinishedMs) {
                    minFinishedMs = elapsedMs;
                    MW_show_log(tr("Fall-short baseline updated: %1 ms").arg(minFinishedMs));
                }
            }

            auto updatedProfile = Configs::dataManager->profilesRepo->GetProfile(entID);
            if (updatedProfile && updatedProfile->connect_time_ms > 0) {
                if (minConnectionTimeMs <= 0 || updatedProfile->connect_time_ms < minConnectionTimeMs) {
                    minConnectionTimeMs = updatedProfile->connect_time_ms;
                    MW_show_log(tr("Fall-short connection baseline updated: %1 ms").arg(minConnectionTimeMs));
                }
            }
        }

        const bool canAutoConnect = completedFully && !stopSpeedtest.load();
        speedtestRunning.store(false);
        runOnUiThread([=, this] {
            refresh_proxy_list({}, true);
            ui->pushButton_cancel_speedtest->setVisible(false);
            ui->pushButton_cancel_speedtest->setEnabled(false);
            MW_show_log(canAutoConnect ? tr("Fall-short speedtest finished!") : tr("Fall-short speedtest interrupted."));

            if (connectMode != SpeedtestConnectMode::None && !canAutoConnect) {
                MW_show_log(tr("Auto connect skipped because speedtest did not finish completely."));
                return;
            }

            int bestId = -1;
            if (connectMode == SpeedtestConnectMode::BestConnectionTime) {
                int bestConn = std::numeric_limits<int>::max();
                for (int id : profileIDs) {
                    auto p = Configs::dataManager->profilesRepo->GetProfile(id);
                    if (!p || p->connect_time_ms <= 0) continue;
                    if (p->connect_time_ms < bestConn) {
                        bestConn = p->connect_time_ms;
                        bestId = id;
                    }
                }
            } else if (connectMode == SpeedtestConnectMode::BestSiteScore) {
                int bestScore = -1;
                for (int id : profileIDs) {
                    auto p = Configs::dataManager->profilesRepo->GetProfile(id);
                    if (!p || p->site_score <= 0) continue;
                    if (p->site_score > bestScore) {
                        bestScore = p->site_score;
                        bestId = id;
                    }
                }
            }

            if (bestId >= 0) {
                profile_start(bestId);
            } else if (connectMode == SpeedtestConnectMode::None && profileToRestore >= 0) {
                profile_start(profileToRestore);
            }
        });
    });
}

void MainWindow::querySpeedtest(QDateTime lastProxyListUpdate, const QMap<QString, int>& tag2entID)
{
    if (tag2entID.empty()) {
        return;
    }
    bool ok;
    auto res = defaultClient->QueryCurrentSpeedTests(&ok);
    if (!ok || !res.is_running.value())
    {
        return;
    }
    auto profile = Configs::dataManager->profilesRepo->GetProfile(tag2entID[QString::fromStdString(res.result.value().outbound_tag.value())]);
    if (profile == nullptr)
    {
        return;
    }
    runOnUiThread([=, this, &lastProxyListUpdate]
    {
        showSpeedtestData = true;
        currentSptProfileName = profile->outbound->name;
        currentTestStatusText = tr("Speedtest");
        currentTestResult = res.result.value();
        UpdateDataView(false); // don't force update if already in correct state

        if (res.result.value().error.value().empty() && !res.result.value().cancelled.value() && lastProxyListUpdate.msecsTo(QDateTime::currentDateTime()) >= 500)
        {
            if (!res.result.value().dl_speed.value().empty()) profile->dl_speed = QString::fromStdString(res.result.value().dl_speed.value());
            if (!res.result.value().ul_speed.value().empty()) profile->ul_speed = QString::fromStdString(res.result.value().ul_speed.value());
            profile->dl_speed_mbps = ParseSpeedToMbps(profile->dl_speed);
            profile->ul_speed_mbps = ParseSpeedToMbps(profile->ul_speed);
            if (profile->connect_time_ms <= 0 && res.result.value().latency.value() > 0) {
                // Connection time is captured from the speedtest cloud connect phase itself.
                profile->connect_time_ms = res.result.value().latency.value();
            }
            if (profile->latency <= 0 && res.result.value().latency.value() > 0) profile->latency = res.result.value().latency.value();
            profile->site_score = CalcSiteScore(profile->connect_time_ms, profile->dl_speed_mbps);
            refresh_proxy_list({profile->id});
            lastProxyListUpdate = QDateTime::currentDateTime();
        }
    });
}


void MainWindow::runSpeedTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID)
{
    if (stopSpeedtest.load()) {
        MW_show_log(tr("Profile speed test aborted"));
        return;
    }

    libcore::SpeedTestRequest req;
    for (const auto &item: outboundTags) {
        req.outbound_tags.push_back(item.toStdString());
    }
    req.config = config.toStdString();
    req.use_default_outbound = useDefault;
    req.test_download = true;
    req.test_upload = false;
    req.simple_download = true;
    req.simple_download_addr = (Configs::dataManager->settingsRepo->simple_dl_url.isEmpty()
        ? QString("https://speed.cloudflare.com/__down?bytes=2000000")
        : Configs::dataManager->settingsRepo->simple_dl_url).toStdString();
    req.test_current = false;
    req.timeout_ms = Configs::dataManager->settingsRepo->speed_test_timeout_ms;
    req.only_country = false;
    req.country_concurrency = 0;
    req.xray_config = xrayConfig.toStdString();
    req.need_xray = !xrayConfig.isEmpty();

    // loop query result
    auto doneMu = new QMutex;
    doneMu->lock();
    runOnNewThread([=,this]
    {
        QDateTime lastProxyListUpdate = QDateTime::currentDateTime();
        while (true) {
            QThread::msleep(100);
            if (doneMu->tryLock())
            {
                break;
            }
            querySpeedtest(lastProxyListUpdate, tag2entID);
        }
        runOnUiThread([=, this]
        {
            showSpeedtestData = false;
            UpdateDataView(true);
        });
        doneMu->unlock();
        delete doneMu;
    });
    bool rpcOK;
    auto result = defaultClient->SpeedTest(&rpcOK, req);
    doneMu->unlock();
    //
    if (!rpcOK || result.results.empty()) return;

    for (const auto &res: result.results) {
        entID = ResolveEntID(tag2entID, entID, res.outbound_tag.value());
        if (entID == -1) {
            MW_show_log(tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }

        auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
        if (ent == nullptr) {
            MW_show_log(tr("Profile manager data is corrupted, try again."));
            continue;
        }

        if (res.cancelled.value()) continue;

        if (res.error.value().empty()) {
            ent->dl_speed = QString::fromStdString(res.dl_speed.value());
            ent->ul_speed = QString::fromStdString(res.ul_speed.value());
            ent->dl_speed_mbps = ParseSpeedToMbps(ent->dl_speed);
            ent->ul_speed_mbps = ParseSpeedToMbps(ent->ul_speed);
            if (res.latency.value() > 0) ent->connect_time_ms = res.latency.value();
            else if (ent->connect_time_ms <= 0) ent->connect_time_ms = -1;
            if (ent->latency <= 0 && res.latency.value() > 0) ent->latency = res.latency.value();
            ent->site_score = CalcSiteScore(ent->connect_time_ms, ent->dl_speed_mbps);
        } else {
            ent->dl_speed = "Error";
            ent->dl_speed_mbps = 0.0;
            if (ent->ul_speed.isEmpty()) ent->ul_speed = "N/A";
            if (ent->ul_speed_mbps < 0.0) ent->ul_speed_mbps = 0.0;
            if (ent->connect_time_ms <= 0) ent->connect_time_ms = -1;
            if (ent->latency <= 0) ent->latency = -1;
            ent->site_score = -2;
            MW_show_log(tr("[%1] speed test error: %2").arg(ent->outbound->DisplayTypeAndName(), QString::fromStdString(res.error.value())));
        }
        Configs::dataManager->profilesRepo->Save(ent);
        int profileId = ent->id;
        runOnUiThread([=, this]() {
            refresh_proxy_list({profileId});
        });
    }
}

void MainWindow::runSpeedTestFallShort(const QString& config, const QString& xrayConfig, bool useDefault,
                                       const QStringList& outboundTags, const QMap<QString, int>& tag2entID,
                                       int entID, int timeoutMs, qint64* elapsedMsOut, bool* successOut, bool* skippedOut)
{
    if (elapsedMsOut) *elapsedMsOut = 0;
    if (successOut) *successOut = false;
    if (skippedOut) *skippedOut = false;

    if (stopSpeedtest.load()) {
        MW_show_log(tr("Profile speed test aborted"));
        return;
    }

    libcore::SpeedTestRequest req;
    for (const auto &item: outboundTags) {
        req.outbound_tags.push_back(item.toStdString());
    }
    req.config = config.toStdString();
    req.use_default_outbound = useDefault;
    req.test_download = true;
    req.test_upload = false;
    req.simple_download = true;
    req.simple_download_addr = Configs::dataManager->settingsRepo->simple_dl_url.toStdString();
    req.test_current = false;
    req.timeout_ms = timeoutMs;
    req.only_country = false;
    req.country_concurrency = 0;
    req.xray_config = xrayConfig.toStdString();
    req.need_xray = !xrayConfig.isEmpty();

    auto doneMu = new QMutex;
    doneMu->lock();
    runOnNewThread([=, this]
    {
        QDateTime lastProxyListUpdate = QDateTime::currentDateTime();
        while (true) {
            QThread::msleep(100);
            if (doneMu->tryLock())
            {
                break;
            }
            querySpeedtest(lastProxyListUpdate, tag2entID);
        }
        runOnUiThread([=, this]
        {
            showSpeedtestData = false;
            UpdateDataView(true);
        });
        doneMu->unlock();
        delete doneMu;
    });

    QElapsedTimer timer;
    timer.start();

    bool rpcOK;
    auto result = defaultClient->SpeedTest(&rpcOK, req);
    doneMu->unlock();
    if (elapsedMsOut) *elapsedMsOut = timer.elapsed();
    if (!rpcOK || result.results.empty()) {
        QList<int> idsToSkip;
        if (entID >= 0) {
            idsToSkip << entID;
        } else {
            for (auto it = tag2entID.constBegin(); it != tag2entID.constEnd(); ++it) {
                idsToSkip << it.value();
            }
        }
        for (int id : idsToSkip) {
            auto ent = Configs::dataManager->profilesRepo->GetProfile(id);
            if (!ent) continue;
            ent->dl_speed = "Skipped";
            ent->dl_speed_mbps = 0.0;
            ent->ul_speed = "N/A";
            ent->ul_speed_mbps = 0.0;
            ent->site_score = -1;
            Configs::dataManager->profilesRepo->Save(ent);
            int profileId = ent->id;
            runOnUiThread([=, this]() {
                refresh_proxy_list({profileId});
            });
            MW_show_log(tr("[%1] fall-short: skipped by timeout threshold (%2 ms)").arg(ent->outbound->DisplayTypeAndName()).arg(timeoutMs));
        }
        if (skippedOut) *skippedOut = !idsToSkip.isEmpty();
        return;
    }

    bool hasSuccess = false;
    for (const auto &res: result.results) {
        entID = ResolveEntID(tag2entID, entID, res.outbound_tag.value());
        if (entID == -1) {
            MW_show_log(tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }

        auto ent = Configs::dataManager->profilesRepo->GetProfile(entID);
        if (ent == nullptr) {
            MW_show_log(tr("Profile manager data is corrupted, try again."));
            continue;
        }

        if (res.cancelled.value()) continue;

        if (res.error.value().empty()) {
            hasSuccess = true;
            ent->dl_speed = QString::fromStdString(res.dl_speed.value());
            ent->ul_speed = QString::fromStdString(res.ul_speed.value());
            ent->dl_speed_mbps = ParseSpeedToMbps(ent->dl_speed);
            ent->ul_speed_mbps = ParseSpeedToMbps(ent->ul_speed);
            if (res.latency.value() > 0) ent->connect_time_ms = res.latency.value();
            else if (ent->connect_time_ms <= 0) ent->connect_time_ms = -1;
            if (ent->latency <= 0 && res.latency.value() > 0) ent->latency = res.latency.value();
            ent->site_score = CalcSiteScore(ent->connect_time_ms, ent->dl_speed_mbps);
        } else {
            const QString err = QString::fromStdString(res.error.value());
            const QString low = err.toLower();
            const bool timedOut = low.contains("timeout") || low.contains("deadline") || low.contains("context deadline exceeded");
            if (timedOut) {
                ent->dl_speed = "Skipped";
                ent->dl_speed_mbps = 0.0;
                ent->ul_speed = "N/A";
                ent->ul_speed_mbps = 0.0;
                ent->site_score = -1;
                if (skippedOut) *skippedOut = true;
                MW_show_log(tr("[%1] fall-short: skipped by timeout threshold (%2 ms)").arg(ent->outbound->DisplayTypeAndName()).arg(timeoutMs));
            } else {
                ent->dl_speed = "Error";
                ent->dl_speed_mbps = 0.0;
                if (ent->ul_speed.isEmpty()) ent->ul_speed = "N/A";
                if (ent->ul_speed_mbps < 0.0) ent->ul_speed_mbps = 0.0;
                if (ent->connect_time_ms <= 0) ent->connect_time_ms = -1;
                if (ent->latency <= 0) ent->latency = -1;
                ent->site_score = -2;
                MW_show_log(tr("[%1] speed test error: %2").arg(ent->outbound->DisplayTypeAndName(), err));
            }
        }
        Configs::dataManager->profilesRepo->Save(ent);
        int profileId = ent->id;
        runOnUiThread([=, this]() {
            refresh_proxy_list({profileId});
        });
    }

    if (successOut) *successOut = hasSuccess;
}

bool MainWindow::set_system_dns(bool set, bool save_set) {
    if (!Configs::dataManager->settingsRepo->enable_dns_server) {
        MW_show_log(tr("You need to enable hijack DNS server first"));
        return false;
    }
    if (!get_elevated_permissions(4)) {
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

    auto ents = get_now_selected_list();
    auto ent = (_id < 0 && !ents.isEmpty()) ? Configs::dataManager->profilesRepo->GetProfile(ents.first()) : Configs::dataManager->profilesRepo->GetProfile(_id);
    if (ent == nullptr) return;

    if (select_mode) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }

    auto group = Configs::dataManager->groupsRepo->GetGroup(ent->gid);
    if (group == nullptr || group->archive) return;

    auto result = Configs::BuildSingBoxConfig(ent);
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
        if (ent->type == "extracore")
        {
            req.need_extra_process = true;
            req.extra_process_path = result->extraCoreData->path.toStdString();
            req.extra_process_args = result->extraCoreData->args.toStdString();
            req.extra_process_conf = result->extraCoreData->config.toStdString();
            req.extra_process_conf_dir = result->extraCoreData->configDir.toStdString();
            req.extra_no_out = result->extraCoreData->noLog;
        }
        //
        bool rpcOK;
        QString error = defaultClient->Start(&rpcOK, req);
        if (!rpcOK) {
            return false;
        }
        if (!error.isEmpty()) {
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
            runOnUiThread([=,this] { MessageBoxWarning("LoadConfig return error", error); });
            return false;
        }
        //
        Stats::trafficLooper->SetEnts(result->outboundEntsForTraffic);
        Stats::trafficLooper->isChain = result->isChained;
        Stats::trafficLooper->loop_enabled = true;
        Stats::connection_lister->suspend = false;

        Configs::dataManager->settingsRepo->UpdateStartedId(ent->id);
        running = ent;

        runOnUiThread([=, this] {
            refresh_status();
            refresh_proxy_list({ent->id});

            auto resp = NetworkRequestHelper::HttpGet("http://ip-api.com/json/", false, true);
            if (resp.error.isEmpty()) {
                QJsonDocument doc = QJsonDocument::fromJson(resp.data);
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();
                    QString ip = obj["query"].toString();
                    QString city = obj["city"].toString();
                    QString countryName = obj["country"].toString();
                    QString countryCode = obj["countryCode"].toString();
                    ui->label_outbound->setText(QString("🌐 %1\n%2 %3, %4").arg(ip, CountryCodeToFlag(countryCode), countryName, city));
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
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=,this] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 10000);

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
        runOnUiThread([=,this] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
        });
    });
}

void MainWindow::set_spmode_system_proxy(bool enable, bool save) {
    if (enable != Configs::dataManager->settingsRepo->spmode_system_proxy) {
        if (enable) {
            auto socks_port = Configs::dataManager->settingsRepo->inbound_socks_port;
            SetSystemProxy(socks_port, socks_port, Configs::dataManager->settingsRepo->proxy_scheme);
        } else {
            ClearSystemProxy();
        }
    }

    if (save) {
        Configs::dataManager->settingsRepo->remember_spmode.removeAll("system_proxy");
        if (enable && Configs::dataManager->settingsRepo->remember_enable) {
            Configs::dataManager->settingsRepo->remember_spmode.append("system_proxy");
        }
        Configs::dataManager->settingsRepo->Save();
    }

    Configs::dataManager->settingsRepo->spmode_system_proxy = enable;
    refresh_status();
}

void MainWindow::profile_stop(bool crash, bool block, bool manual) {
    if (running == nullptr) {
        return;
    }
    auto id = running->id;

    auto profile_stop_stage2 = [=,this] {
        if (!crash) {
            bool rpcOK;
            QString error = defaultClient->Stop(&rpcOK);
            if (rpcOK && !error.isEmpty()) {
                runOnUiThread([=,this] { MessageBoxWarning(tr("Stop return error"), error); });
                return false;
            } else if (!rpcOK) {
                return false;
            }
        }
        return true;
    };

    if (!mu_stopping.tryLock()) {
        return;
    }

    UpdateConnectionListWithRecreate({});

    runOnNewThread([=, this] {
        Stats::trafficLooper->loop_enabled = false;
        Stats::connection_lister->suspend = true;
        Stats::trafficLooper->loop_mutex.lock();
        Stats::trafficLooper->UpdateAll();
        Stats::trafficLooper->loop_mutex.unlock();

        QMessageBox* restartMsgbox;
        MessageBoxTimer* restartMsgboxTimer;
        runOnUiThread([=, this, &restartMsgbox, &restartMsgboxTimer] {
            restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                             QMessageBox::Yes | QMessageBox::No, this);
            connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
            restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);
        }, true);

        // do stop
        MW_show_log(">>>>>>>> " + tr("Stopping profile %1").arg(running->outbound->DisplayTypeAndName()));
        if (!profile_stop_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to stop, please restart the program."));
        }

        if (manual) Configs::dataManager->settingsRepo->UpdateStartedId(-1919);
        Configs::dataManager->settingsRepo->need_keep_vpn_off = false;
        running = nullptr;

        runOnUiThread([=, this, &restartMsgboxTimer, &restartMsgbox] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();

            refresh_status();
            refresh_proxy_list({id});
            ui->label_outbound->setText("");

            mu_stopping.unlock();
        }, true);
    }, block);
}
