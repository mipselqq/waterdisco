#include "include/ui/mainWindow/TestRunner.h"

#include "include/ui/mainwindow.h"

#include "include/api/RPC.h"
#include "include/configs/generate.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/stats/traffic/TrafficStatsManager.hpp"

#include <QCoreApplication>
#include <QPointer>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QElapsedTimer>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

using namespace API;

namespace {
    int configuredTestConcurrency() {
        const int configured = Configs::dataManager->settingsRepo->test_concurrent;
        return qMax(1, configured > 0 ? configured : 100);
    }

    // One shared test box per chunk; parallel probes capped by the same setting.
    int configuredTestBatchSize() {
        return configuredTestConcurrency();
    }
    constexpr int kLatencyPollIntervalMs = 200;
    constexpr int kSpeedPollIntervalMs = 100;
    constexpr auto kWaterdiscoProbeUrl = "https://speed.cloudflare.com/__down?bytes=2000000";

    QList<int> withoutDisabled(const QList<int>& profileIDs) {
        QList<int> filtered;
        filtered.reserve(profileIDs.size());
        for (int id : profileIDs) {
            if (!Configs::dataManager->settingsRepo->IsProfileDisabled(id)) {
                filtered.append(id);
            }
        }
        return filtered;
    }

    QList<int> withoutAutoSelectors(const QList<int>& profileIDs) {
        const auto selectors = Configs::dataManager->profilesRepo->GetProfileIdsByType("autoselector");
        if (selectors.isEmpty()) return profileIDs;
        const QSet<int> skip(selectors.begin(), selectors.end());
        QList<int> filtered;
        filtered.reserve(profileIDs.size());
        for (int id : profileIDs) {
            if (!skip.contains(id)) filtered << id;
        }
        return filtered;
    }

    // StopTest / runBatch abort. Fall-short and HTTP timeouts use "context canceled"
    // or "deadline exceeded" and must still be recorded as Skip/Error.
    bool isTestAborted(const QString& error) {
        return error.contains(QLatin1String("test aborted"));
    }

    bool isVpnProfile(const std::shared_ptr<Configs::Profile>& ent) {
        return ent != nullptr && (ent->type == "openvpn" || ent->type == "openconnect");
    }

    constexpr int kVpnStatusWaitMs = 10000;

    // An empty tag map means a single-profile box, so the result must be `fallback`.
    int resolveEntID(const QMap<QString, int>& tag2entID, const std::string& tag, int fallback) {
        if (tag2entID.isEmpty()) return fallback;
        return tag2entID.value(QString::fromStdString(tag), -1);
    }

    void persistProfileIDs(const QList<int>& ids) {
        if (ids.isEmpty()) return;
        Configs::dataManager->profilesRepo->SaveBatch(
            Configs::dataManager->profilesRepo->GetProfileBatch(ids));
    }

    void persistProfiles(const QList<std::shared_ptr<Configs::Profile>>& profiles) {
        Configs::dataManager->profilesRepo->SaveBatch(profiles);
    }

    QString testFinishLog(const QString& message, qint64 elapsedMs) {
        return QStringLiteral("%1 (%2)").arg(message, FormatElapsedHms(elapsedMs));
    }

    bool isCanceledProbeError(const QString& error) {
        return error.contains(QStringLiteral("context canceled"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("context deadline exceeded"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("Client.Timeout"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("i/o timeout"), Qt::CaseInsensitive);
    }

    QString compactProbeError(QString error) {
        error = error.trimmed();
        if (error.startsWith(QLatin1String("Get \""))) {
            const int sep = error.indexOf(QLatin1String("\": "));
            if (sep > 0) error = error.mid(sep + 3);
        }
        return error;
    }

    bool usedFallShortLimit(bool fallShort, int limitMs, int defaultLimitMs) {
        return fallShort && limitMs > 0 && limitMs < defaultLimitMs;
    }

    bool connectionProbeAlreadyResolved(const Configs::Profile& ent) {
        return ent.connect_time_ms > 0
            || ent.performance_test_status == Configs::PerformanceTestStatus::Skipped
            || ent.performance_test_status == Configs::PerformanceTestStatus::Error
            || ent.performance_test_status == Configs::PerformanceTestStatus::Success;
    }

    bool downloadAlreadyResolved(const Configs::Profile& ent) {
        return ent.performance_test_status == Configs::PerformanceTestStatus::Success
            || ent.performance_test_status == Configs::PerformanceTestStatus::Skipped
            || ent.performance_test_status == Configs::PerformanceTestStatus::Error;
    }

    void noteFasterMs(std::atomic<qint64>* best, qint64 observed) {
        if (!best || observed <= 0) return;
        qint64 current = best->load();
        while (current <= 0 || observed < current) {
            if (best->compare_exchange_weak(current, observed)) break;
        }
    }

    QString connectionProbeLimitReason(bool fallShort, int limitMs, int defaultLimitMs) {
        if (usedFallShortLimit(fallShort, limitMs, defaultLimitMs)) {
            return MainWindow::tr("Fall-short: connection limit exceeded (%1 ms)").arg(limitMs);
        }
        if (limitMs > 0) {
            return MainWindow::tr("Connection probe timed out (%1 ms)").arg(limitMs);
        }
        return MainWindow::tr("Connection probe failed");
    }

    QString downloadProbeLimitReason(bool fallShort, int limitMs, int defaultLimitMs) {
        if (usedFallShortLimit(fallShort, limitMs, defaultLimitMs)) {
            return MainWindow::tr("Fall-short: download limit exceeded (%1 ms)").arg(limitMs);
        }
        if (limitMs > 0) {
            return MainWindow::tr("Download timed out (%1 ms)").arg(limitMs);
        }
        return MainWindow::tr("Download failed");
    }

    Configs::PerformanceStatusDetail connectionProbeLimitDetail(bool fallShort, int limitMs,
                                                               int defaultLimitMs,
                                                               qint64 measuredMs = -1) {
        Configs::PerformanceStatusDetail detail;
        detail.reason = connectionProbeLimitReason(fallShort, limitMs, defaultLimitMs);
        detail.measuredMs = measuredMs;
        detail.thresholdMs = usedFallShortLimit(fallShort, limitMs, defaultLimitMs) ? limitMs : defaultLimitMs;
        return detail;
    }

    Configs::PerformanceStatusDetail downloadFailureDetail(bool fallShort, int limitMs,
                                                           int defaultLimitMs,
                                                           const QString& coreError = {},
                                                           qint64 measuredMs = -1,
                                                           double measuredMbps = -1.0) {
        Configs::PerformanceStatusDetail detail;
        detail.measuredMs = measuredMs;
        detail.measuredMbps = measuredMbps;
        if (!coreError.isEmpty() && !isCanceledProbeError(coreError)) {
            detail.reason = compactProbeError(coreError);
            return detail;
        }
        detail.reason = downloadProbeLimitReason(fallShort, limitMs, defaultLimitMs);
        detail.thresholdMs = usedFallShortLimit(fallShort, limitMs, defaultLimitMs) ? limitMs : defaultLimitMs;
        return detail;
    }

    Configs::PerformanceStatusDetail speedTestBuildError(const QString& message) {
        Configs::PerformanceStatusDetail detail;
        detail.reason = message;
        return detail;
    }

    Configs::PerformanceStatusDetail speedTestInvalidResult(int connectionTimeMs, double rxMbps) {
        Configs::PerformanceStatusDetail detail;
        detail.reason = MainWindow::tr("Invalid speed test result");
        if (connectionTimeMs > 0) detail.measuredMs = connectionTimeMs;
        if (rxMbps > 0.0) detail.measuredMbps = rxMbps;
        return detail;
    }

    QString statusName(Configs::PerformanceTestStatus status) {
        switch (status) {
        case Configs::PerformanceTestStatus::Untested: return QStringLiteral("untested");
        case Configs::PerformanceTestStatus::Success: return QStringLiteral("success");
        case Configs::PerformanceTestStatus::Skipped: return QStringLiteral("skipped");
        case Configs::PerformanceTestStatus::Error: return QStringLiteral("error");
        }
        return QStringLiteral("?");
    }


    // Target is deduced, not named: access control applies to naming a private type.
    template <typename Req, typename Target>
    void fillCommonTestReq(Req& req, const Target& target) {
        for (const auto& tag : target.outboundTags) req.outbound_tags.push_back(tag.toStdString());
        req.config = target.coreConfig.toStdString();
        req.use_default_outbound = target.useDefaultOutbound;
        req.xray_config = target.xrayConfig.toStdString();
        req.need_xray = !target.xrayConfig.isEmpty();
        req.xray_outbound_dns_strategy = target.xrayDnsStrategy.toStdString();
        for (const auto& xc : target.xrayFullConfigs) req.xray_full_configs.push_back(xc.toStdString());
    }

    // Stopping does not join: the poll may itself sit in a 30s RPC and must not stall the batch.
    class ResultPoller {
    public:
        ResultPoller(std::function<void()> tick, int intervalMs, std::atomic<bool>* abort)
            : stop_(std::make_shared<std::atomic<bool>>(false)) {
            runOnNewThread([stop = stop_, tick = std::move(tick), intervalMs, abort] {
                while (!stop->load() && (abort == nullptr || !abort->load())) {
                    QThread::msleep(intervalMs);
                    if (stop->load() || (abort != nullptr && abort->load())) break;
                    tick();
                }
            });
        }

        ~ResultPoller() { stop_->store(true); }

        ResultPoller(const ResultPoller&) = delete;
        ResultPoller& operator=(const ResultPoller&) = delete;

    private:
        std::shared_ptr<std::atomic<bool>> stop_;
    };
}

TestRunner::Target TestRunner::buildTarget(const std::shared_ptr<Configs::Profile>& profile, QString* error) const {
    auto build = Configs::BuildTestConfig({profile});
    if (!build->error.isEmpty()) {
        if (error) *error = build->error;
        return {};
    }

    Target target;
    if (build->fullConfigs.contains(profile->id)) {
        target.coreConfig = build->fullConfigs[profile->id];
        target.useDefaultOutbound = true;
        target.entID = profile->id;
        return target;
    }
    if (build->outboundTags.empty()) {
        if (error) *error = MainWindow::tr("No testable outbound was generated");
        return {};
    }
    target.coreConfig = QJsonObject2QString(build->coreConfig, false);
    target.xrayConfig = build->isXrayNeeded ? QJsonObject2QString(build->xrayConfig, false) : QString();
    target.xrayFullConfigs = build->xrayFullConfigs;
    target.outboundTags = build->outboundTags;
    target.tag2entID = build->tag2entID;
    target.entID = profile->id;
    return target;
}

QList<TestRunner::Target> TestRunner::buildTargets(
    const QList<std::shared_ptr<Configs::Profile>>& profiles, QString* error) const {
    auto build = Configs::BuildTestConfig(profiles);
    if (!build->error.isEmpty()) {
        if (error) *error = build->error;
        return {};
    }

    QList<Target> targets;
    if (!build->outboundTags.empty()) {
        Target target;
        target.coreConfig = QJsonObject2QString(build->coreConfig, false);
        target.xrayConfig = build->isXrayNeeded ? QJsonObject2QString(build->xrayConfig, false) : QString();
        target.xrayFullConfigs = build->xrayFullConfigs;
        target.outboundTags = build->outboundTags;
        target.tag2entID = build->tag2entID;
        targets.append(target);
    }
    for (auto it = build->fullConfigs.cbegin(); it != build->fullConfigs.cend(); ++it) {
        Target target;
        target.coreConfig = it.value();
        target.useDefaultOutbound = true;
        target.entID = it.key();
        targets.append(target);
    }
    return targets;
}

void TestRunner::applyConnectionResult(const std::shared_ptr<Configs::Profile>& ent,
                                       const libcore::URLTestResp& res,
                                       std::atomic<qint64>* bestConnectionMs, bool fallShort,
                                       int limitMs, int defaultTimeoutMs) {
    if (!ent) return;
    QMutexLocker lock(&rankedApplyMu_);
    const QString error = QString::fromStdString(res.error.value());
    if (isTestAborted(error) || stopRequested_.load()) {
        return;
    }

    // A later poller tick must not clobber a TTFB we already accepted. Tags were
    // reused across chunks (proxy-1-0, …), so a stale QueryURLTest could apply
    // the next batch's result onto this profile.
    if (ent->connect_time_ms > 0) {
        if (error.isEmpty() && res.latency_ms.value() > 0
            && res.latency_ms.value() < ent->connect_time_ms) {
            ent->connect_time_ms = res.latency_ms.value();
            noteFasterMs(bestConnectionMs, ent->connect_time_ms);
        }
        return;
    }

    if (connectionProbeAlreadyResolved(*ent)) return;

    if (error.isEmpty() && res.latency_ms.value() > 0) {
        ent->connect_time_ms = res.latency_ms.value();
        noteFasterMs(bestConnectionMs, ent->connect_time_ms);
        return;
    }

    const qint64 measuredMs = res.latency_ms.value() > 0 ? res.latency_ms.value() : -1;
    const bool canceled = isCanceledProbeError(error);
    if (!error.isEmpty() && !canceled) {
        Configs::PerformanceStatusDetail detail;
        detail.reason = compactProbeError(error);
        detail.measuredMs = measuredMs;
        ent->MarkPerformanceError(detail);
        return;
    }

    const auto detail = connectionProbeLimitDetail(fallShort, limitMs, defaultTimeoutMs, measuredMs);
    if (fallShort && (canceled || usedFallShortLimit(fallShort, limitMs, defaultTimeoutMs))) {
        ent->MarkPerformanceSkipped(detail);
    } else {
        ent->MarkPerformanceError(detail);
    }
}

void TestRunner::runRankedUrlProbe(const Target& target, bool fallShort, int limitMs,
                                   int defaultTimeoutMs, std::atomic<qint64>* bestConnectionMs) {
    if (stopRequested_.load()) return;

    libcore::TestReq req;
    fillCommonTestReq(req, target);
    req.url = kWaterdiscoProbeUrl;
    req.max_concurrency = configuredTestConcurrency();
    req.test_timeout_ms = std::max(1, limitMs);
    req.dynamic_fall_short = fallShort;
    if (fallShort && bestConnectionMs) {
        req.fall_short_best_ms = static_cast<int32_t>(std::max<qint64>(0, bestConnectionMs->load()));
    }

    bool rpcOK = false;
    QString coreError;
    libcore::TestResp result;
    {
        ResultPoller poller([this, gen = sessionGen_.load(), tag2entID = target.tag2entID, fallback = target.entID,
                             bestConnectionMs, fallShort, limitMs, defaultTimeoutMs] {
            if (sessionGen_.load() != gen || stopRequested_.load()) return;
            bool ok = false;
            const auto resp = defaultClient->QueryURLTest(&ok);
            if (!ok || resp.results.empty()) return;

            QList<int> updated;
            for (const auto& res : resp.results) {
                const int entid = resolveEntID(tag2entID, res.outbound_tag.value(), fallback);
                if (entid == -1) continue;
                auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (!ent) continue;
                applyConnectionResult(ent, res, bestConnectionMs, fallShort, limitMs, defaultTimeoutMs);
                updated << entid;
            }
            if (updated.isEmpty()) return;
            postUi([updated](MainWindow* mw) {
                mw->UpdateDataView();
                mw->refresh_proxy_list(updated);
            });
        }, kLatencyPollIntervalMs, &stopRequested_);

        result = defaultClient->Test(&rpcOK, req, &coreError);
    }

    if (!rpcOK) {
        if (!stopRequested_.load()) mw_->handleXrayGeoAssetError(coreError, contextName(target.entID));
        return;
    }
    for (const auto& res : result.results) {
        const int entid = resolveEntID(target.tag2entID, res.outbound_tag.value(), target.entID);
        if (entid == -1) continue;
        auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
        if (!ent) continue;
        applyConnectionResult(ent, res, bestConnectionMs, fallShort, limitMs, defaultTimeoutMs);
    }
}

bool TestRunner::runRankedConnectionPretest(const QList<int>& profileIDs, bool fallShort,
                                            qint64* bestConnectionMs, int* skipped, bool* hadErrors,
                                            const QString& stage) {
    const QString progressStage = stage.isEmpty()
        ? MainWindow::tr("Connection Test")
        : stage;
    const int configuredTimeout = std::max(1, Configs::dataManager->settingsRepo->url_test_timeout_ms);
    std::atomic<qint64> best{bestConnectionMs && *bestConnectionMs > 0 ? *bestConnectionMs : 0};
    // One shared box per chunk of configuredTestBatchSize(), probed at the same
    // concurrency. Fewer core starts than a per-profile box.
    const int chunkSize = configuredTestBatchSize();

    for (int i = 0; i < profileIDs.size(); i += chunkSize) {
        if (stopRequested_.load()) return false;
        const auto slice = profileIDs.mid(i, chunkSize);
        auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(slice);
        if (!profiles.isEmpty()) {
            const int skip = skipped ? *skipped : 0;
            updateRankedProgress(progressStage, profiles.front(),
                                 qMax(0, i - skip), skip, profileIDs.size());
        }

        QString buildError;
        const auto targets = buildTargets(profiles, &buildError);
        if (!buildError.isEmpty()) {
            MW_show_log(MainWindow::tr("Failed to build test config for batch: ") + buildError);
            for (const auto& profile : profiles) {
                profile->MarkPerformanceError(speedTestBuildError(buildError));
                if (hadErrors) *hadErrors = true;
            }
            persistProfiles(profiles);
            postUi([n = slice.size()](MainWindow* mw) { mw->dataViewHtmlGenerator_.addTestProgress(n); });
            continue;
        }

        for (const auto& target : targets) {
            const int limitMs = fallShort
                ? Configs::RankedFallShortConnectionTimeoutMs(configuredTimeout, best.load())
                : configuredTimeout;

            if (stopRequested_.load()) {
                persistProfileIDs(slice);
                return false;
            }
            runRankedUrlProbe(target, fallShort, limitMs, configuredTimeout, &best);
        }

        const int leftoverLimitMs = fallShort
            ? Configs::RankedFallShortConnectionTimeoutMs(configuredTimeout, best.load())
            : configuredTimeout;
        for (int id : slice) {
            auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
            if (!profile || profile->connect_time_ms > 0) continue;
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Error) {
                if (hadErrors) *hadErrors = true;
                continue;
            }
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Skipped) {
                if (skipped) ++*skipped;
                continue;
            }
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Success) continue;
            const bool inTargetMap = [&] {
                for (const auto& target : targets) {
                    if (target.entID == id) return true;
                    for (auto it = target.tag2entID.cbegin(); it != target.tag2entID.cend(); ++it) {
                        if (it.value() == id) return true;
                    }
                }
                return false;
            }();
            const auto detail = connectionProbeLimitDetail(fallShort, leftoverLimitMs, configuredTimeout);
            if (fallShort) {
                profile->MarkPerformanceSkipped(detail);
                if (skipped) ++*skipped;
            } else {
                profile->MarkPerformanceError(detail);
                if (hadErrors) *hadErrors = true;
                if (skipped) ++*skipped;
            }
        }
        persistProfileIDs(slice);
        int leftoverSkip = 0, leftoverError = 0, leftoverOk = 0, leftoverEmptyTip = 0;
        for (int id : slice) {
            auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
            if (!profile) continue;
            if (profile->connect_time_ms > 0) {
                leftoverOk++;
                continue;
            }
            if (profile->performance_test_status == Configs::PerformanceTestStatus::Skipped) leftoverSkip++;
            else if (profile->performance_test_status == Configs::PerformanceTestStatus::Error) leftoverError++;
            if ((profile->performance_test_status == Configs::PerformanceTestStatus::Skipped
                 || profile->performance_test_status == Configs::PerformanceTestStatus::Error)
                && profile->DisplayRxSpeedTooltip().isEmpty()) {
                leftoverEmptyTip++;
            }
        }
        const int skip = skipped ? *skipped : 0;
        const int processed = i + slice.size();
        if (!profiles.isEmpty()) {
            updateRankedProgress(progressStage, profiles.front(),
                                 qMax(0, processed - skip), skip, profileIDs.size());
        } else {
            postUi([n = slice.size()](MainWindow* mw) { mw->dataViewHtmlGenerator_.addTestProgress(n); });
        }
        postUi([slice](MainWindow* mw) { mw->refresh_proxy_list(slice); });
    }

    if (bestConnectionMs) {
        const qint64 value = best.load();
        if (value > 0) *bestConnectionMs = value;
    }
    return !stopRequested_.load();
}

void TestRunner::applyDownloadResult(const std::shared_ptr<Configs::Profile>& profile,
                                     const libcore::SpeedTestResult& res, bool fallShort, int limitMs,
                                     int defaultTimeoutMs, std::atomic<qint64>* bestConnectionMs,
                                     std::atomic<qint64>* bestDownloadMs,
                                     int* tested, int* skipped, bool* hadErrors) {
    if (!profile) return;
    QMutexLocker lock(&rankedApplyMu_);

    const QString error = QString::fromStdString(res.error.value());
    const qint64 elapsedMs = res.elapsed_ms.value() > 0 ? res.elapsed_ms.value() : -1;
    const int latencyMs = res.latency.value();
    if (latencyMs > 0) noteFasterMs(bestConnectionMs, latencyMs);

    const bool alreadyDone = downloadAlreadyResolved(*profile);
    if (alreadyDone) {
        if (error.isEmpty() && elapsedMs > 0) noteFasterMs(bestDownloadMs, elapsedMs);
        return;
    }

    if (isTestAborted(error) || stopRequested_.load()) return;

    creditTraffic(profile, QString::fromStdString(res.outbound_tag.value()),
                  res.ul_bytes.value(), res.dl_bytes.value());

    const bool canceled = res.cancelled.value() || isCanceledProbeError(error);
    if (!error.isEmpty() || res.cancelled.value()) {
        const auto detail = downloadFailureDetail(fallShort, limitMs, defaultTimeoutMs, error, elapsedMs);
        if (fallShort && (canceled || usedFallShortLimit(fallShort, limitMs, defaultTimeoutMs))) {
            profile->MarkPerformanceSkipped(detail);
            if (skipped) ++*skipped;
        } else {
            profile->MarkPerformanceError(detail);
            if (hadErrors) *hadErrors = true;
            if (skipped) ++*skipped;
        }
        return;
    }

    const QString speed = QString::fromStdString(res.dl_speed.value());
    const double rxMbps = Configs::ParseSpeedMbps(speed);
    const int connectionTime = latencyMs > 0 ? latencyMs : profile->connect_time_ms;
    if (rxMbps <= 0.0 || connectionTime <= 0) {
        profile->MarkPerformanceError(speedTestInvalidResult(connectionTime, rxMbps));
        if (hadErrors) *hadErrors = true;
        if (skipped) ++*skipped;
        return;
    }
    profile->SetPerformanceResult(connectionTime, rxMbps, speed);
    noteFasterMs(bestConnectionMs, connectionTime);
    noteFasterMs(bestDownloadMs, elapsedMs);
    if (tested) ++*tested;
}

bool TestRunner::runRankedDownloadTarget(const Target& target, int timeoutMs, bool fallShort,
                                         int defaultTimeoutMs, std::atomic<qint64>* bestConnectionMs,
                                         std::atomic<qint64>* bestDownloadMs,
                                         int* tested, int* skipped, bool* hadErrors) {
    if (stopRequested_.load()) return false;

    libcore::SpeedTestRequest req;
    fillCommonTestReq(req, target);
    req.simple_download = true;
    req.simple_download_addr = kWaterdiscoProbeUrl;
    req.timeout_ms = std::max(1, timeoutMs);
    // Sequential 2 MiB transfers: parallel downloads would contend and change Mbps.
    // Fall-short still reads/updates global bests after every result in this batch.
    req.country_concurrency = 1;
    req.dynamic_fall_short = fallShort;
    if (fallShort) {
        req.fall_short_best_ms = static_cast<int32_t>(
            std::max<qint64>(0, bestConnectionMs ? bestConnectionMs->load() : 0));
        req.fall_short_best_download_ms = static_cast<int32_t>(
            std::max<qint64>(0, bestDownloadMs ? bestDownloadMs->load() : 0));
    }

    bool rpcOK = false;
    QString coreError;
    libcore::SpeedTestResponse result;

    auto markUnresolved = [&](int id) {
        auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
        if (!profile || downloadAlreadyResolved(*profile)) return;
        const auto detail = downloadFailureDetail(fallShort, timeoutMs, defaultTimeoutMs,
                                                  !rpcOK ? coreError : QString());
        if (fallShort && (isCanceledProbeError(coreError)
                          || usedFallShortLimit(fallShort, timeoutMs, defaultTimeoutMs))) {
            profile->MarkPerformanceSkipped(detail);
            if (skipped) ++*skipped;
        } else {
            profile->MarkPerformanceError(detail);
            if (hadErrors) *hadErrors = true;
            if (skipped) ++*skipped;
        }
    };

    {
        ResultPoller poller([this, gen = sessionGen_.load(), tag2entID = target.tag2entID,
                             fallback = target.entID, fallShort, timeoutMs, defaultTimeoutMs,
                             bestConnectionMs, bestDownloadMs, tested, skipped, hadErrors] {
            if (sessionGen_.load() != gen || stopRequested_.load()) return;
            bool ok = false;
            const auto resp = defaultClient->QueryCurrentSpeedTests(&ok);
            if (!ok) return;
            QList<int> updated;
            for (const auto& res : resp.completed) {
                const int entid = resolveEntID(tag2entID, res.outbound_tag.value(), fallback);
                if (entid == -1) continue;
                auto profile = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (!profile) continue;
                applyDownloadResult(profile, res, fallShort, timeoutMs, defaultTimeoutMs,
                                    bestConnectionMs, bestDownloadMs, tested, skipped, hadErrors);
                updated << entid;
            }
            if (updated.isEmpty()) return;
            persistProfileIDs(updated);
            postUi([updated](MainWindow* mw) {
                mw->UpdateDataView();
                mw->refresh_proxy_list(updated);
            });
        }, kSpeedPollIntervalMs, &stopRequested_);

        result = defaultClient->SpeedTest(&rpcOK, req, &coreError);
    }

    auto targetIDs = [&] {
        QList<int> ids;
        if (!target.tag2entID.isEmpty()) ids = target.tag2entID.values();
        else if (target.entID >= 0) ids << target.entID;
        return ids;
    };

    if (!rpcOK) {
        if (stopRequested_.load()) return false;
        mw_->handleXrayGeoAssetError(coreError, contextName(target.entID));
        const auto ids = targetIDs();
        for (int id : ids) markUnresolved(id);
        persistProfileIDs(ids);
        postUi([n = ids.size()](MainWindow* mw) { mw->dataViewHtmlGenerator_.addTestProgress(n); });
        return true;
    }

    QList<std::shared_ptr<Configs::Profile>> dirty;
    for (const auto& res : result.results) {
        const int entid = resolveEntID(target.tag2entID, res.outbound_tag.value(), target.entID);
        if (entid == -1) continue;
        auto profile = Configs::dataManager->profilesRepo->GetProfile(entid);
        if (!profile) continue;
        const QString error = QString::fromStdString(res.error.value());
        applyDownloadResult(profile, res, fallShort, timeoutMs, defaultTimeoutMs,
                            bestConnectionMs, bestDownloadMs, tested, skipped, hadErrors);
        dirty.append(profile);
        if (isTestAborted(error) || stopRequested_.load()) {
            persistProfiles(dirty);
            return false;
        }
        postUi([id = profile->id](MainWindow* mw) { mw->refresh_proxy_list({id}); });
    }
    for (int id : targetIDs()) {
        auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
        if (!profile || downloadAlreadyResolved(*profile)) continue;
        markUnresolved(id);
        dirty.append(profile);
    }
    persistProfiles(dirty);
    postUi([n = dirty.size()](MainWindow* mw) { mw->dataViewHtmlGenerator_.addTestProgress(n); });
    return !stopRequested_.load();
}

void TestRunner::updateRankedProgress(const QString& stage, const std::shared_ptr<Configs::Profile>& profile,
                                      int tested, int skipped, int total) {
    if (!profile) return;
    const QString name = profile->outbound ? profile->outbound->name : profile->name;
    postUi([stage, name, tested, skipped, total](MainWindow* mw) {
        mw->dataViewHtmlGenerator_.setRankedSpeedtestProgress(stage, name, tested, skipped, total);
        mw->UpdateDataView();
    });
}

int TestRunner::bestSiteScoreProfile(const QList<int>& profileIDs) const {
    int bestID = -1;
    int bestScore = -1;
    for (const int id : profileIDs) {
        const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
        if (!profile || profile->performance_test_status != Configs::PerformanceTestStatus::Success) continue;
        if (profile->site_score > bestScore) {
            bestScore = profile->site_score;
            bestID = id;
        }
    }
    return bestID;
}

void TestRunner::runRankedSpeedTests(const QList<int>& requestedIDs, RankedStartMode mode,
                                     bool connectBestSiteScore, bool fallShort) {
    const auto profileIDs = withoutAutoSelectors(withoutDisabled(requestedIDs));
    if (profileIDs.isEmpty()) return;

    runOnNewThread([this, profileIDs, mode, connectBestSiteScore, fallShort] {
        takeSession();
        sessionGen_.fetch_add(1);
        stopRequested_.store(false);
        { QMutexLocker lock(&creditMu_); credited_.clear(); }

        postUi([](MainWindow* mw) {
            mw->ui->pushButton_cancel_speedtest->setVisible(true);
            mw->ui->pushButton_cancel_speedtest->setEnabled(true);
        });

        QElapsedTimer elapsed;
        elapsed.start();

        postUi([count = profileIDs.size()](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.seedSpeedTest(count);
            mw->UpdateDataView();
        });

        // Snapshot last-run metrics before clearing. Ordering after a clear is a
        // no-op for saved-site-score mode.
        QList<Configs::RankedScheduleRow> snapshot;
        snapshot.reserve(profileIDs.size());
        for (int id : profileIDs) {
            Configs::RankedScheduleRow row;
            row.id = id;
            if (const auto profile = Configs::dataManager->profilesRepo->GetProfile(id)) {
                row.connectTimeMs = profile->connect_time_ms;
                row.siteScore = profile->site_score;
            }
            snapshot.append(row);
        }

        QList<int> downloadOrder = profileIDs;
        QList<int> pretestOrder;
        if (mode == RankedStartMode::BySavedSiteScore) {
            downloadOrder = Configs::OrderRankedBySavedSiteScore(snapshot);
        } else if (mode == RankedStartMode::ByConnectionTime) {
            pretestOrder = Configs::OrderRankedConnectionPretest(snapshot);
        }

        const auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDs);
        for (const auto& profile : profiles) profile->ClearPerformanceTestResults();
        Configs::dataManager->profilesRepo->SaveBatch(profiles);
        postUi([profileIDs](MainWindow* mw) { mw->refresh_proxy_list(profileIDs); });

        int tested = 0;
        int skipped = 0;
        qint64 bestConnectionMs = -1;
        qint64 bestDownloadMs = -1;
        bool completed = true;
        bool hadErrors = false;
        if (fallShort) {
            for (const auto& row : snapshot) {
                if (row.connectTimeMs > 0 && (bestConnectionMs <= 0 || row.connectTimeMs < bestConnectionMs)) {
                    bestConnectionMs = row.connectTimeMs;
                }
            }
        }
        const int defaultDownloadTimeout = std::max(1, Configs::dataManager->settingsRepo->speed_test_timeout_ms);
        const QString downloadStage = fallShort
            ? MainWindow::tr("Speedtest (Fall-short)")
            : MainWindow::tr("Speedtest");

        if (mode == RankedStartMode::ByConnectionTime) {
            const QString connectionStage = fallShort
                ? MainWindow::tr("Connection Test (Fall-short)")
                : MainWindow::tr("Connection Test");
            completed = runRankedConnectionPretest(pretestOrder, fallShort, &bestConnectionMs,
                                                   &skipped, &hadErrors, connectionStage);
            if (!completed) {
                // Fall through to the shared restore path.
            } else {
                QList<Configs::RankedScheduleRow> survivors;
                survivors.reserve(profileIDs.size());
                for (int id : profileIDs) {
                    const auto profile = Configs::dataManager->profilesRepo->GetProfile(id);
                    if (!profile || profile->connect_time_ms <= 0) continue;
                    survivors.append({id, profile->connect_time_ms, 0});
                    if (bestConnectionMs <= 0 || profile->connect_time_ms < bestConnectionMs) {
                        bestConnectionMs = profile->connect_time_ms;
                    }
                }
                if (survivors.isEmpty()) {
                    MW_show_log(MainWindow::tr("Connection-time precheck produced no valid candidates; speedtest skipped."));
                    completed = true;
                    downloadOrder.clear();
                } else {
                    downloadOrder = Configs::OrderRankedByConnectionTime(survivors);
                }
            }
        }

        std::atomic<qint64> bestConn{bestConnectionMs > 0 ? bestConnectionMs : 0};
        std::atomic<qint64> bestDl{bestDownloadMs > 0 ? bestDownloadMs : 0};

        if (completed && !stopRequested_.load()) {
            for (int i = 0; i < downloadOrder.size(); i += configuredTestBatchSize()) {
                if (stopRequested_.load()) { completed = false; break; }
                const auto slice = downloadOrder.mid(i, configuredTestBatchSize());
                auto sliceProfiles = Configs::dataManager->profilesRepo->GetProfileBatch(slice);
                if (!sliceProfiles.isEmpty()) {
                    updateRankedProgress(downloadStage, sliceProfiles.front(),
                                         tested, skipped, profileIDs.size());
                }
                QString buildError;
                const auto targets = buildTargets(sliceProfiles, &buildError);
                if (!buildError.isEmpty()) {
                    MW_show_log(MainWindow::tr("Failed to build batch test config: ") + buildError);
                    for (const auto& profile : sliceProfiles) {
                        profile->MarkPerformanceError(speedTestBuildError(buildError));
                    }
                    persistProfiles(sliceProfiles);
                    postUi([n = slice.size()](MainWindow* mw) { mw->dataViewHtmlGenerator_.addTestProgress(n); });
                    continue;
                }
                for (const auto& target : targets) {
                    if (stopRequested_.load()) { completed = false; break; }
                    const int timeout = fallShort
                        ? Configs::RankedFallShortDownloadTimeoutMs(
                              defaultDownloadTimeout, bestDl.load(), bestConn.load())
                        : defaultDownloadTimeout;
                    if (!runRankedDownloadTarget(target, timeout, fallShort, defaultDownloadTimeout,
                                                 &bestConn, &bestDl, &tested, &skipped, &hadErrors)) {
                        completed = false;
                        break;
                    }
                }
                if (!completed) break;
                postUi([slice](MainWindow* mw) { mw->refresh_proxy_list(slice); });
            }
        } else if (stopRequested_.load()) {
            completed = false;
        }

        bestConnectionMs = bestConn.load();
        bestDownloadMs = bestDl.load();

        const bool canChooseProfile = completed && !stopRequested_.load();
        const int bestID = canChooseProfile && connectBestSiteScore
            ? bestSiteScoreProfile(profileIDs) : -1;
        const bool cancelled = stopRequested_.load();
        const qint64 elapsedMs = elapsed.elapsed();
        session_.unlock();
        postUi([this, profileIDs, bestID, canChooseProfile, elapsedMs, cancelled](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.clearTestSections();
            mw->UpdateDataView(true);
            if (!cancelled) mw->refresh_proxy_list(profileIDs);
            mw->ui->pushButton_cancel_speedtest->setVisible(false);
            mw->ui->pushButton_cancel_speedtest->setEnabled(false);
            MW_show_log(testFinishLog(
                canChooseProfile ? MainWindow::tr("Speedtest finished!") : MainWindow::tr("Speedtest interrupted."),
                elapsedMs));
            if (bestID >= 0 && (!mw->running || mw->running->id != bestID)) {
                mw->profile_start(bestID);
            }
        });
    });
}


void TestRunner::postUi(const std::function<void(MainWindow*)>& fn) const {
    QPointer<MainWindow> guard(mw_);
    runOnUiThread([guard, fn] {
        if (guard) fn(guard.data());
    });
}

bool TestRunner::isRunning() {
    if (!session_.tryLock()) return true;
    session_.unlock();
    return false;
}

void TestRunner::stop() {
    stopRequested_.store(true);
    sessionGen_.fetch_add(1);
    defaultClient->FailInFlightCalls();
    auto notifyCore = [] {
        bool ok = false;
        defaultClient->StopTests(&ok);
    };
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        runOnNewThread(notifyCore);
    } else {
        notifyCore();
    }
}

void TestRunner::takeSession() {
    if (session_.tryLock()) return;
    stop();
    session_.lock();
}

QString TestRunner::contextName(int entID) const {
    if (entID != -1) {
        if (auto e = Configs::dataManager->profilesRepo->GetProfile(entID)) return e->outbound->DisplayTypeAndName();
    }
    return MainWindow::tr("a tested profile");
}

void TestRunner::applyUrlResult(const std::shared_ptr<Configs::Profile>& ent, const libcore::URLTestResp& res,
                                const QHash<QString, bool>* vpnConnected) {
    const auto error = QString::fromStdString(res.error.value());
    if (error.isEmpty()) {
        ent->SetLatency(res.latency_ms.value());
    } else if (isTestAborted(error)) {
        ent->SetLatency(0);
    } else if (vpnConnected != nullptr && isVpnProfile(ent)
               && vpnConnected->value(QString::fromStdString(res.outbound_tag.value()), false)) {
        ent->SetLatency(Configs::kLatencyConnectOnly);
    } else {
        ent->SetLatency(-1);
        MW_show_log(MainWindow::tr("[%1] test error: %2").arg(ent->outbound->DisplayTypeAndName(), error));
    }
    Configs::dataManager->profilesRepo->Save(ent);
}

void TestRunner::applyIpResult(const std::shared_ptr<Configs::Profile>& ent, const libcore::IPTestRes& res,
                               bool live) {
    const auto error = QString::fromStdString(res.error.value());
    if (error.isEmpty()) {
        const QString newIp = QString::fromStdString(res.ip.value());
        const QString newCountry = QString::fromStdString(res.country_code.value());
        if (live && ent->ip_out == newIp && ent->test_country == newCountry) {
            return;
        }
        ent->ip_out = newIp;
        ent->test_country = newCountry;
    } else {
        if (!isTestAborted(error) && !live) {
            MW_show_log(MainWindow::tr("[%1] IP test error: %2").arg(ent->outbound->DisplayTypeAndName(), error));
        }
        if (!live) {
            ent->ip_out.clear();
            ent->test_country.clear();
        } else {
            return;
        }
    }
    Configs::dataManager->profilesRepo->Save(ent);
    postUi([this](MainWindow* mw) { mw->refresh_status(); });
}

void TestRunner::runUrlProbe(const Target& target) {
    if (stopRequested_.load()) {
        MW_show_log(MainWindow::tr("Profile test aborted"));
        return;
    }

    libcore::TestReq req;
    fillCommonTestReq(req, target);
    req.url = Configs::dataManager->settingsRepo->test_latency_url.toStdString();
    req.max_concurrency = configuredTestConcurrency();
    req.test_timeout_ms = Configs::dataManager->settingsRepo->url_test_timeout_ms;

    // The test box dies with the RPC, so the verdict has to be asked for up front.
    for (auto it = target.tag2entID.cbegin(); it != target.tag2entID.cend(); ++it) {
        if (isVpnProfile(Configs::dataManager->profilesRepo->GetProfile(it.value()))) {
            req.vpn_endpoint_tags.push_back(it.key().toStdString());
        }
    }
    if (!req.vpn_endpoint_tags.empty()) req.vpn_status_timeout_ms = kVpnStatusWaitMs;

    bool rpcOK = false;
    QString coreError;
    libcore::TestResp result;
    {
        // The core's buffer is global: a poll can take a sibling's results, reclaimed below.
        ResultPoller poller([this, gen = sessionGen_.load(), tag2entID = target.tag2entID] {
            if (sessionGen_.load() != gen || stopRequested_.load()) return;
            bool ok = false;
            const auto resp = defaultClient->QueryURLTest(&ok);
            if (!ok || resp.results.empty()) return;

            QList<int> updated;
            for (const auto& res : resp.results) {
                const int entid = resolveEntID(tag2entID, res.outbound_tag.value(), -1);
                if (entid == -1) continue;
                auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (ent == nullptr) continue;
                applyUrlResult(ent, res);
                updated << entid;
            }
            if (updated.isEmpty()) return;
            postUi([updated](MainWindow* mw) {
                mw->dataViewHtmlGenerator_.addTestProgress(updated.size());
                mw->UpdateDataView();
                mw->refresh_proxy_list(updated);
            });
        }, kLatencyPollIntervalMs, &stopRequested_);

        result = defaultClient->Test(&rpcOK, req, &coreError);
    }

    if (!rpcOK || result.results.empty()) {
        // A failed Test RPC yields no per-result errors, so inspect it here for the
        // geo-asset prompt - the same flow profile start uses.
        if (!rpcOK && !stopRequested_.load()) mw_->handleXrayGeoAssetError(coreError, contextName(target.entID));
        return;
    }

    QHash<QString, bool> vpnConnected;
    for (const auto& st : result.vpn_status) {
        vpnConnected.insert(QString::fromStdString(st.tag.value()), st.connected.value());
    }

    for (const auto& res : result.results) {
        const int entid = resolveEntID(target.tag2entID, res.outbound_tag.value(), target.entID);
        if (entid == -1) {
            MW_show_log(MainWindow::tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }
        auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
        if (ent == nullptr) {
            MW_show_log(MainWindow::tr("Profile manager data is corrupted, try again."));
            continue;
        }
        applyUrlResult(ent, res, &vpnConnected);
    }
}

bool TestRunner::runIpProbe(const Target& target) {
    if (stopRequested_.load()) {
        MW_show_log(MainWindow::tr("Profile test aborted"));
        return false;
    }

    libcore::IPTestRequest req;
    fillCommonTestReq(req, target);
    req.test_current = target.testCurrent;
    req.max_concurrency = configuredTestConcurrency();
    const int timeoutMs = target.testCurrent
        ? qMax(Configs::dataManager->settingsRepo->url_test_timeout_ms, 5000)
        : Configs::dataManager->settingsRepo->url_test_timeout_ms;
    req.test_timeout_ms = timeoutMs;

    bool rpcOK = false;
    QString coreError;
    libcore::IPTestResp result;
    {
        ResultPoller poller([this, gen = sessionGen_.load(), tag2entID = target.tag2entID,
                             live = target.testCurrent] {
            if (live) return;
            if (sessionGen_.load() != gen || stopRequested_.load()) return;
            bool ok = false;
            const auto resp = defaultClient->QueryIPTest(&ok);
            if (!ok || resp.results.empty()) return;

            QList<int> updated;
            for (const auto& res : resp.results) {
                const int entid = resolveEntID(tag2entID, res.outbound_tag.value(), -1);
                if (entid == -1) continue;
                auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
                if (ent == nullptr) continue;
                applyIpResult(ent, res);
                updated << entid;
            }
            if (updated.isEmpty()) return;
            postUi([updated](MainWindow* mw) {
                mw->dataViewHtmlGenerator_.addTestProgress(updated.size());
                mw->UpdateDataView();
                mw->refresh_proxy_list(updated);
            });
        }, kLatencyPollIntervalMs, &stopRequested_);

        result = defaultClient->IPTest(&rpcOK, req, &coreError);
    }

    if (!rpcOK || result.results.empty()) {
        // Detect missing Xray geo assets from a failed IPTest RPC (see runUrlProbe).
        if (!rpcOK && !stopRequested_.load()) mw_->handleXrayGeoAssetError(coreError, contextName(target.entID));
        return false;
    }

    bool success = false;
    for (const auto& res : result.results) {
        const int entid = resolveEntID(target.tag2entID, res.outbound_tag.value(), target.entID);
        if (entid == -1) {
            MW_show_log(MainWindow::tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }
        auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
        if (ent == nullptr) {
            MW_show_log(MainWindow::tr("Profile manager data is corrupted, try again."));
            continue;
        }
        applyIpResult(ent, res, target.testCurrent);
        if (QString::fromStdString(res.error.value()).isEmpty()
            && !QString::fromStdString(res.ip.value()).trimmed().isEmpty()) {
            success = true;
        }
    }
    return success;
}

void TestRunner::runConnectionTimeTests(const QList<int>& requestedIDs) {
    const auto profileIDs = withoutAutoSelectors(withoutDisabled(requestedIDs));
    if (profileIDs.isEmpty()) return;
    const bool fallShort = Configs::dataManager->settingsRepo->speed_test_fall_short;

    runOnNewThread([this, profileIDs, fallShort] {
        takeSession();
        sessionGen_.fetch_add(1);
        stopRequested_.store(false);
        { QMutexLocker lock(&creditMu_); credited_.clear(); }

        postUi([](MainWindow* mw) {
            mw->ui->pushButton_cancel_speedtest->setVisible(true);
            mw->ui->pushButton_cancel_speedtest->setEnabled(true);
        });

        QElapsedTimer elapsed;
        elapsed.start();

        postUi([count = profileIDs.size()](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.seedSpeedTest(count);
            mw->UpdateDataView();
        });

        QList<Configs::RankedScheduleRow> snapshot;
        snapshot.reserve(profileIDs.size());
        for (int id : profileIDs) {
            Configs::RankedScheduleRow row;
            row.id = id;
            if (const auto profile = Configs::dataManager->profilesRepo->GetProfile(id)) {
                row.connectTimeMs = profile->connect_time_ms;
                row.siteScore = profile->site_score;
            }
            snapshot.append(row);
        }
        const QList<int> pretestOrder = Configs::OrderRankedConnectionPretest(snapshot);

        auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDs);
        for (const auto& profile : profiles) profile->connect_time_ms = 0;
        Configs::dataManager->profilesRepo->SaveBatch(profiles);
        postUi([profileIDs](MainWindow* mw) { mw->refresh_proxy_list(profileIDs); });

        qint64 bestConnectionMs = -1;
        if (fallShort) {
            for (const auto& row : snapshot) {
                if (row.connectTimeMs > 0 && (bestConnectionMs <= 0 || row.connectTimeMs < bestConnectionMs)) {
                    bestConnectionMs = row.connectTimeMs;
                }
            }
        }

        int skipped = 0;
        bool hadErrors = false;
        const QString stage = fallShort
            ? MainWindow::tr("Connection Test (Fall-short)")
            : MainWindow::tr("Connection Test");
        const bool completed = runRankedConnectionPretest(pretestOrder, fallShort, &bestConnectionMs,
                                                            &skipped, &hadErrors, stage);

        const qint64 elapsedMs = elapsed.elapsed();
        const bool cancelled = stopRequested_.load();
        session_.unlock();
        postUi([profileIDs, completed, elapsedMs, cancelled](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.clearTestSections();
            mw->UpdateDataView(true);
            if (!cancelled) mw->refresh_proxy_list(profileIDs);
            mw->ui->pushButton_cancel_speedtest->setVisible(false);
            mw->ui->pushButton_cancel_speedtest->setEnabled(false);
            MW_show_log(testFinishLog(
                completed ? MainWindow::tr("Connection test finished!") : MainWindow::tr("Connection test interrupted."),
                elapsedMs));
        });
    });
}

void TestRunner::runUrlTests(const QList<int>& profileIDs, const std::function<void()>& onFinished) {
    runLatencyGroup(LatencyKind::Url, profileIDs, onFinished);
}

void TestRunner::runIpTests(const QList<int>& profileIDs) {
    runLatencyGroup(LatencyKind::Ip, profileIDs, {});
}

void TestRunner::runCurrentIpTest(int profileID, const std::function<void(bool)>& finished) {
    if (profileID < 0) {
        if (finished) finished(false);
        return;
    }
    if (!session_.tryLock()) {
        if (finished) finished(false);
        return;
    }
    sessionGen_.fetch_add(1);
    testingCurrent_.store(true);

    runOnNewThread([this, profileID, finished] {
        stopRequested_.store(false);
        Target target;
        target.entID = profileID;
        target.testCurrent = true;
        const bool success = runIpProbe(target);
        const bool cancelled = stopRequested_.load();
        if (!cancelled) {
            if (const auto profile = Configs::dataManager->profilesRepo->GetProfile(profileID)) {
                if (success) {
                    mw_->connectionProbeFailStreak.store(0);
                    profile->connection_test_status = Configs::ConnectionTestStatus::Success;
                } else if (mw_->connectionProbeFailStreak.fetch_add(1) + 1 >= 2) {
                    profile->connection_test_status = Configs::ConnectionTestStatus::Error;
                }
            }
        }
        testingCurrent_.store(false);
        session_.unlock();
        postUi([this, profileID](MainWindow* mw) {
            if (mw->isVisible() && !mw->isMinimized()) {
                mw->refresh_proxy_list({profileID});
            }
        });
        if (finished) {
            runOnUiThread([finished, success] { finished(success); });
        }
    });
}

void TestRunner::runLatencyGroup(LatencyKind kind, const QList<int>& requestedIDs,
                                 const std::function<void()>& onFinished) {
    const bool isUrl = kind == LatencyKind::Url;
    const auto panelKind = isUrl ? DataViewHtmlGenerator::LatencyTestPanelState::Kind::Url
                                 : DataViewHtmlGenerator::LatencyTestPanelState::Kind::Ip;
    // Must fire on every exit path — a caller may be blocked on it.
    const auto finish = [onFinished] { if (onFinished) onFinished(); };

    const auto profileIDs = withoutAutoSelectors(withoutDisabled(requestedIDs));
    if (profileIDs.isEmpty()) {
        finish();
        return;
    }

    runOnNewThread([this, profileIDs, panelKind, isUrl, finish]() {
        takeSession();
        sessionGen_.fetch_add(1);
        stopRequested_.store(false);
        postUi([panelKind, count = profileIDs.size()](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.seedLatencyTest(panelKind, count);
            mw->UpdateDataView(true);
        });

        auto runBatch = [this, isUrl](const QList<std::shared_ptr<Configs::Profile>>& profileSlice, const QList<int>& ids) {
            auto buildObject = Configs::BuildTestConfig(profileSlice);
            if (!buildObject->error.isEmpty()) {
                MW_show_log(MainWindow::tr("Failed to build test config for batch: ") + buildObject->error);
                return;
            }

            // xray-full tags live in outboundTags, so those configs add no separate tests.
            const int testCount = buildObject->fullConfigs.size() + (buildObject->outboundTags.empty() ? 0 : 1);
            if (testCount == 0) return;

            QSemaphore batchDone;
            const auto probe = [this, isUrl, &batchDone](const Target& target) {
                mw_->parallelCoreCallPool->start([this, isUrl, target, &batchDone] {
                    const QSemaphoreReleaser releaser(batchDone);
                    if (isUrl) runUrlProbe(target);
                    else runIpProbe(target);
                });
            };

            for (const auto& entID : buildObject->fullConfigs.keys()) {
                Target target;
                target.coreConfig = buildObject->fullConfigs[entID];
                target.useDefaultOutbound = true;
                target.entID = entID;
                probe(target);
            }
            if (!buildObject->outboundTags.empty()) {
                Target target;
                target.coreConfig = QJsonObject2QString(buildObject->coreConfig, false);
                target.xrayConfig = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, false) : "";
                target.xrayFullConfigs = buildObject->xrayFullConfigs;
                target.outboundTags = buildObject->outboundTags;
                target.tag2entID = buildObject->tag2entID;
                target.xrayDnsStrategy = buildObject->xrayDnsStrategy;
                probe(target);
            }
            batchDone.acquire(testCount);

            MW_show_log(isUrl ? "URL test for batch done." : "IP test for batch done.");
            runOnUiThread([=, this] {
                mw_->refresh_proxy_list(ids);
            });
        };

        std::shared_ptr<Configs::Group> currentGroup;
        for (int i = 0; i < profileIDs.length(); i += configuredTestBatchSize()) {
            if (stopRequested_.load()) break;
            const auto profileIDsSlice = profileIDs.mid(i, configuredTestBatchSize());
            auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDsSlice);
            if (isUrl && !currentGroup && !profiles.isEmpty()) {
                currentGroup = Configs::dataManager->groupsRepo->GetGroup(profiles[0]->gid);
            }
            runBatch(profiles, profileIDsSlice);
        }

        postUi([](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.clearTestSections();
            mw->UpdateDataView(true);
        });
        session_.unlock();
        // Signalled with the session free so a waiter can start work of its own.
        finish();

        // Auto-clear prunes on latency, so it is a URL-test notion only.
        if (currentGroup != nullptr && currentGroup->auto_clear_unavailable) {
            MW_show_log("URL test finished, clearing unavailable profiles...");
            runOnUiThread([=, this] {
               mw_->clearUnavailableProfiles(false, profileIDs);
            });
        }
        MW_show_log(isUrl ? MainWindow::tr("URL test finished!") : MainWindow::tr("IP test finished!"));
    });
}

void TestRunner::runSpeedTests(const QList<int>& requestedIDs, bool testCurrent)
{
    // A live-connection test stays valid for a running selector: it measures
    // whichever member actually carries traffic.
    const auto profileIDs = testCurrent
        ? withoutDisabled(requestedIDs)
        : withoutAutoSelectors(withoutDisabled(requestedIDs));
    if (profileIDs.isEmpty() && !testCurrent) {
        return;
    }

    runOnNewThread([this, profileIDs, testCurrent]() {
        takeSession();
        sessionGen_.fetch_add(1);
        testingCurrent_.store(testCurrent);
        stopRequested_.store(false);
        QElapsedTimer elapsed;
        elapsed.start();
        // Fresh per-tag byte baselines for this speed-test session.
        { QMutexLocker lk(&creditMu_); credited_.clear(); }
        if (!testCurrent)
        {
            postUi([count = profileIDs.size()](MainWindow* mw) {
                mw->dataViewHtmlGenerator_.seedSpeedTest(count);
                mw->UpdateDataView(true);
            });
            auto runBatch = [this](const QList<std::shared_ptr<Configs::Profile>>& profileSlice) {
                auto buildObject = Configs::BuildTestConfig(profileSlice);
                if (!buildObject->error.isEmpty()) {
                    MW_show_log(MainWindow::tr("Failed to build batch test config: ") + buildObject->error);
                    return;
                }

                for (auto it = buildObject->fullConfigs.cbegin(); it != buildObject->fullConfigs.cend(); ++it) {
                    Target target;
                    target.coreConfig = it.value();
                    target.useDefaultOutbound = true;
                    target.entID = it.key();
                    runSpeedProbe(target);
                }

                if (!buildObject->outboundTags.empty()) {
                    Target target;
                    target.coreConfig = QJsonObject2QString(buildObject->coreConfig, false);
                    target.xrayConfig = buildObject->isXrayNeeded ? QJsonObject2QString(buildObject->xrayConfig, true) : "";
                    target.xrayFullConfigs = buildObject->xrayFullConfigs;
                    target.outboundTags = buildObject->outboundTags;
                    target.tag2entID = buildObject->tag2entID;
                    target.xrayDnsStrategy = buildObject->xrayDnsStrategy;
                    runSpeedProbe(target);
                }
            };
            // Batch size matches concurrency; range is enforced in settings (0–10000).
            const int stepSize = configuredTestBatchSize();
            for (int i = 0; i < profileIDs.length(); i += stepSize) {
                if (stopRequested_.load()) break;
                const auto profileIDsSlice = profileIDs.mid(i, stepSize);
                auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(profileIDsSlice);
                runBatch(profiles);
            }
        } else
        {
            postUi([](MainWindow* mw) { mw->dataViewHtmlGenerator_.seedSpeedTest(1); });
            Target target;
            target.testCurrent = true;
            runSpeedProbe(target);
            testingCurrent_.store(false);
        }
        const qint64 elapsedMs = elapsed.elapsed();
        session_.unlock();
        postUi([=, this, profileIDs, elapsedMs, cancelled = stopRequested_.load()](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.clearTestSections();
            mw->UpdateDataView(true);
            if (!cancelled) mw->refresh_proxy_list(profileIDs);
            MW_show_log(testFinishLog(
                cancelled ? MainWindow::tr("Speedtest interrupted.") : MainWindow::tr("Speedtest finished!"),
                elapsedMs));
        });
    });
}

void TestRunner::creditTraffic(const std::shared_ptr<Configs::Profile>& profile, const QString& tag, qint64 curUp, qint64 curDown)
{
    if (profile == nullptr || tag.isEmpty()) return;
    if (Configs::dataManager->settingsRepo->disable_traffic_stats) return;
    QMutexLocker lk(&creditMu_);
    auto& base = credited_[tag];
    const qint64 dUp = curUp >= base.first ? curUp - base.first : curUp;
    const qint64 dDown = curDown >= base.second ? curDown - base.second : curDown;
    base = qMakePair(curUp, curDown);
    if (dUp <= 0 && dDown <= 0) return;

    Stats::trafficStatsManager->AddConfigDelta(profile->id, dUp, dDown);
    Stats::trafficStatsManager->AddAppDelta(Stats::SPEEDTEST_APP_NAME, "", dUp, dDown);

    profile->traffic_uplink += dUp;
    profile->traffic_downlink += dDown;
    Configs::dataManager->profilesRepo->SaveTraffic(profile);
}

void TestRunner::pollSpeedTest(const QMap<QString, int>& tag2entID, bool testCurrent)
{
    bool ok = false;
    const auto res = defaultClient->QueryCurrentSpeedTests(&ok);
    if (!ok || !res.is_running.value())
    {
        return;
    }
    const libcore::SpeedTestResult result = res.result.value();
    const auto tag = QString::fromStdString(result.outbound_tag.value());
    // value(tag, -1), not operator[]: a const QMap yields 0 for a missing key.
    auto profile = testCurrent ? mw_->running
                               : Configs::dataManager->profilesRepo->GetProfile(tag2entID.value(tag, -1));
    if (profile == nullptr)
    {
        return;
    }
    creditTraffic(profile, tag, result.ul_bytes.value(), result.dl_bytes.value());
    postUi([profile, result](MainWindow* mw) {
        if (result.error.value().empty() && !result.cancelled.value()) {
            if (!result.dl_speed.value().empty()) {
                const QString speed = QString::fromStdString(result.dl_speed.value());
                const int connectionTime = result.latency.value() > 0
                    ? result.latency.value() : profile->connect_time_ms;
                profile->SetPerformanceResult(connectionTime, Configs::ParseSpeedMbps(speed), speed);
            }
            if (!result.ul_speed.value().empty()) profile->ul_speed = QString::fromStdString(result.ul_speed.value());
            if (profile->latency <= 0 && result.latency.value() > 0) profile->SetLatency(result.latency.value());
            if (!result.server_country.value().empty()) {
                profile->test_country = CountryNameToCode(QString::fromStdString(result.server_country.value()));
            }
            mw->refresh_proxy_list({profile->id});
        }
    });
}

void TestRunner::pollCountryTest(const QMap<QString, int>& tag2entID, bool testCurrent)
{
    bool ok = false;
    const auto res = defaultClient->QueryCountryTestResults(&ok);
    if (!ok || res.results.empty())
    {
        return;
    }
    for (const auto& result : res.results)
    {
        const auto tag = QString::fromStdString(result.outbound_tag.value());
        auto profile = testCurrent ? mw_->running
                                   : Configs::dataManager->profilesRepo->GetProfile(tag2entID.value(tag, -1));
        if (profile == nullptr)
        {
            continue;
        }
        postUi([profile, result](MainWindow* mw) {
            if (result.error.value().empty() && !result.cancelled.value()) {
                if (profile->latency <= 0 && result.latency.value() > 0) profile->SetLatency(result.latency.value());
                if (!result.server_country.value().empty()) {
                    profile->test_country = CountryNameToCode(QString::fromStdString(result.server_country.value()));
                }
                mw->refresh_proxy_list({profile->id});
            }
        });
    }
    postUi([](MainWindow* mw) {
        mw->dataViewHtmlGenerator_.addTestProgress();
        mw->UpdateDataView();
    });
}

void TestRunner::runSpeedProbe(const Target& target)
{
    if (stopRequested_.load()) {
        MW_show_log(MainWindow::tr("Profile speed test aborted"));
        return;
    }

    const auto speedtestConf = Configs::dataManager->settingsRepo->speed_test_mode;
    libcore::SpeedTestRequest req;
    fillCommonTestReq(req, target);
    req.test_download = speedtestConf == Configs::TestConfig::FULL || speedtestConf == Configs::TestConfig::DL;
    req.test_upload = speedtestConf == Configs::TestConfig::FULL || speedtestConf == Configs::TestConfig::UL;
    req.simple_download = speedtestConf == Configs::TestConfig::SIMPLEDL;
    req.simple_download_addr = Configs::dataManager->settingsRepo->simple_dl_url.toStdString();
    req.test_current = target.testCurrent;
    req.timeout_ms = Configs::dataManager->settingsRepo->speed_test_timeout_ms;
    req.only_country = speedtestConf == Configs::TestConfig::COUNTRY;
    req.country_concurrency = configuredTestConcurrency();

    // Country sweep ticks per landed result instead; see pollCountryTest.
    if (speedtestConf != Configs::TestConfig::COUNTRY) {
        const int profileCount = target.tag2entID.isEmpty() ? 1 : target.tag2entID.size();
        postUi([profileCount](MainWindow* mw) {
            mw->dataViewHtmlGenerator_.addTestProgress(profileCount);
            mw->UpdateDataView();
        });
    }

    const int contextID = target.testCurrent ? (mw_->running ? mw_->running->id : -1) : target.entID;

    bool rpcOK = false;
    QString coreError;
    libcore::SpeedTestResponse result;
    {
        ResultPoller poller([this, gen = sessionGen_.load(), tag2entID = target.tag2entID, testCurrent = target.testCurrent, speedtestConf] {
            if (sessionGen_.load() != gen || stopRequested_.load()) return;
            if (speedtestConf == Configs::TestConfig::COUNTRY) pollCountryTest(tag2entID, testCurrent);
            else pollSpeedTest(tag2entID, testCurrent);
        }, kSpeedPollIntervalMs, &stopRequested_);

        result = defaultClient->SpeedTest(&rpcOK, req, &coreError);
    }

    if (!rpcOK || result.results.empty()) {
        // Detect missing Xray geo assets from a failed SpeedTest RPC (see runUrlProbe).
        if (!rpcOK && !stopRequested_.load()) mw_->handleXrayGeoAssetError(coreError, contextName(contextID));
        return;
    }

    for (const auto& res : result.results) {
        // An xray-full config is its own box with no tag map, so it must be entID.
        const int entid = target.testCurrent
                              ? (mw_->running ? mw_->running->id : -1)
                              : resolveEntID(target.tag2entID, res.outbound_tag.value(), target.entID);
        if (entid == -1) {
            MW_show_log(MainWindow::tr("Something is very wrong, the subject ent cannot be found!"));
            continue;
        }

        auto ent = Configs::dataManager->profilesRepo->GetProfile(entid);
        if (ent == nullptr) {
            MW_show_log(MainWindow::tr("Profile manager data is corrupted, try again."));
            continue;
        }

        creditTraffic(ent, QString::fromStdString(res.outbound_tag.value()),
                      res.ul_bytes.value(), res.dl_bytes.value());

        if (res.cancelled.value()) continue;

        const auto error = QString::fromStdString(res.error.value());
        if (error.isEmpty()) {
            const QString speed = QString::fromStdString(res.dl_speed.value());
            ent->SetPerformanceResult(res.latency.value(), Configs::ParseSpeedMbps(speed), speed);
            ent->ul_speed = QString::fromStdString(res.ul_speed.value());
            if (ent->latency <= 0 && res.latency.value() > 0) ent->SetLatency(res.latency.value());
            if (!res.server_country.value().empty()) ent->test_country = CountryNameToCode(QString::fromStdString(res.server_country.value()));
        } else {
            ent->MarkPerformanceError(downloadFailureDetail(false, -1, -1, error));
            ent->SetLatency(-1);
            ent->test_country = "";
            MW_show_log(MainWindow::tr("[%1] speed test error: %2").arg(ent->outbound->DisplayTypeAndName(), error));
        }
        Configs::dataManager->profilesRepo->Save(ent);
    }
}
