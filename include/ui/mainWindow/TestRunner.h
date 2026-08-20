#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

#ifndef Q_MOC_RUN
#include <core/server/gen/libcore.pb.h>
#endif

#include "include/database/entities/Profile.h"

class MainWindow;

// Owns profile measuring: URL latency, egress IP and speed. Not a QObject: queued
// signals would reorder the synchronous progress path, and tr() would change context.
class TestRunner {
public:
    enum class RankedStartMode { ByConnectionTime, BySavedSiteScore };

    explicit TestRunner(MainWindow* mw) : mw_(mw) {}

    TestRunner(const TestRunner&) = delete;
    TestRunner& operator=(const TestRunner&) = delete;

    // `onFinished` fires on every exit path, so a caller may block on it.
    void runUrlTests(const QList<int>& profileIDs, const std::function<void()>& onFinished = {});

    void runIpTests(const QList<int>& profileIDs);

    // Concurrent Cloudflare TTFB probe used by ranked speedtest; same panel and
    // cancel path as speedtest, honors speed_test_fall_short.
    void runConnectionTimeTests(const QList<int>& profileIDs);

    // Probe the egress of the currently running core. `finished` runs on the
    // UI thread and reports whether the active profile produced a real IP.
    void runCurrentIpTest(int profileID, const std::function<void(bool)>& finished = {});

    void runSpeedTests(const QList<int>& profileIDs, bool testCurrent = false);

    // Ranked 2 MiB Cloudflare download. ByConnectionTime prepends a concurrent
    // TTFB pretest then downloads fastest-this-run first. BySavedSiteScore
    // takes TTFB from the download. Last-run values only change order.
    void runRankedSpeedTests(const QList<int>& profileIDs, RankedStartMode mode,
                             bool connectBestSiteScore, bool fallShort);

    void stop();

    bool isRunning();

    // A profile stop has to cancel the test before tearing the instance down.
    bool isTestingCurrent() const { return testingCurrent_.load(); }

private:
    enum class LatencyKind { Url, Ip };

    struct Target {
        QString coreConfig;
        QString xrayConfig;
        QStringList xrayFullConfigs;
        QStringList outboundTags;
        QMap<QString, int> tag2entID;
        int entID = -1;
        // Not derivable from an empty outboundTags: a test-current run leaves
        // both empty but must let the core pick "proxy" over the config default.
        bool useDefaultOutbound = false;
        bool testCurrent = false;
    };

    void runLatencyGroup(LatencyKind kind, const QList<int>& requestedIDs,
                         const std::function<void()>& onFinished);

    void runUrlProbe(const Target& target);

    bool runIpProbe(const Target& target);

    void runSpeedProbe(const Target& target);

    struct RankedProbeResult {
        bool completed = false;
        bool success = false;
        bool skipped = false;
        int connectionTimeMs = 0;
        qint64 elapsedMs = 0;
        QString error;
    };

    Target buildTarget(const std::shared_ptr<Configs::Profile>& profile, QString* error) const;
    QList<Target> buildTargets(const QList<std::shared_ptr<Configs::Profile>>& profiles,
                               QString* error) const;
    RankedProbeResult runDownloadProbe(const std::shared_ptr<Configs::Profile>& profile,
                                       int timeoutMs, bool fallShort, int defaultTimeoutMs);
    bool runRankedDownloadTarget(const Target& target, int timeoutMs, bool fallShort,
                                 int defaultTimeoutMs, qint64* elapsedMs,
                                 int* tested, int* skipped, bool* hadErrors);
    bool runRankedConnectionPretest(const QList<int>& profileIDs, bool fallShort,
                                    qint64* bestConnectionMs, int* skipped, bool* hadErrors,
                                    const QString& stage = QString());
    void runRankedUrlProbe(const Target& target, bool fallShort, int timeoutMs,
                           std::atomic<qint64>* bestConnectionMs);
    void applyConnectionResult(const std::shared_ptr<Configs::Profile>& ent,
                               const libcore::URLTestResp& res, std::atomic<qint64>* bestConnectionMs);
    void updateRankedProgress(const QString& stage, const std::shared_ptr<Configs::Profile>& profile,
                              int tested, int skipped, int total);
    int bestSiteScoreProfile(const QList<int>& profileIDs) const;

    // Shared by the live progress poll and the final pass, which must not drift.
    void applyUrlResult(const std::shared_ptr<Configs::Profile>& ent, const libcore::URLTestResp& res);

    void applyIpResult(const std::shared_ptr<Configs::Profile>& ent, const libcore::IPTestRes& res);

    QString contextName(int entID) const;

    void pollSpeedTest(const QMap<QString, int>& tag2entID, bool testCurrent);

    void pollCountryTest(const QMap<QString, int>& tag2entID, bool testCurrent);

    void creditTraffic(const std::shared_ptr<Configs::Profile>& profile, const QString& tag,
                       qint64 curUp, qint64 curDown);

    MainWindow* mw_;

    // Held for a whole sweep, so it must never double as a per-batch latch.
    QMutex session_;
    // A poll thread is not joined, so a late tick must not drain the next sweep.
    std::atomic<quint64> sessionGen_ = 0;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<bool> testingCurrent_ = false;

    // Tests dial the outbound directly and bypass the clash tracker, so their
    // bytes are counted only here, diffed per tag against the last report.
    QMutex creditMu_;
    QHash<QString, QPair<qint64, qint64>> credited_;
};
