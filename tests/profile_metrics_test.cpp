#include "include/database/entities/ProfileMetrics.h"

#include <cassert>

int main() {
    using namespace Configs;

    assert(CalculateSiteScore(0, 100.0) == 0);
    assert(CalculateSiteScore(100, 0.0) == 0);
    assert(CalculateSiteScore(100, 100.0) == 92);
    assert(CalculateSiteScore(250, 200.0) == 92);
    assert(CalculateSiteScore(1000, 200.0) == 75);
    assert(CalculateSiteScore(100, 10.0) < CalculateSiteScore(100, 100.0));
    assert(CalculateSiteScore(500, 100.0) < CalculateSiteScore(100, 100.0));
    assert(ParseSpeedMbps(QStringLiteral("1 Gbps")) == 1000.0);
    assert(ParseSpeedMbps(QStringLiteral("12.5 Mbps")) == 12.5);
    assert(ParseSpeedMbps(QStringLiteral("500 Kbps")) == 0.5);
    assert(ParseSpeedMbps(QStringLiteral("Error")) == 0.0);

    assert(PerformanceStatusRank(PerformanceTestStatus::Success) == 0);
    assert(PerformanceStatusRank(PerformanceTestStatus::Untested) == 1);
    assert(PerformanceStatusRank(PerformanceTestStatus::Skipped) == 2);
    assert(PerformanceStatusRank(PerformanceTestStatus::Error) == 3);

    assert(PerformanceComesBefore(PerformanceTestStatus::Success, 1.0,
                                  PerformanceTestStatus::Untested, 100.0, false));
    assert(PerformanceComesBefore(PerformanceTestStatus::Untested, 0.0,
                                  PerformanceTestStatus::Skipped, 0.0, false));
    assert(PerformanceComesBefore(PerformanceTestStatus::Skipped, 0.0,
                                  PerformanceTestStatus::Error, 0.0, true));
    assert(PerformanceComesBefore(PerformanceTestStatus::Success, 10.0,
                                  PerformanceTestStatus::Success, 20.0, false));
    assert(PerformanceComesBefore(PerformanceTestStatus::Success, 20.0,
                                  PerformanceTestStatus::Success, 10.0, true));
    assert(!PerformanceComesBefore(PerformanceTestStatus::Success, 10.0,
                                   PerformanceTestStatus::Success, 10.0, false));

    // --- Ranked speedtest schedule (model order, not UI) ---
    // TestRunner snapshots these rows, then calls the same helpers below.

    // No saved metrics: ranked helpers keep selection order.
    const QList<RankedScheduleRow> noSavedMetrics = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    assert(RankedScheduleIds(noSavedMetrics) == (QList<int>{1, 2, 3}));
    assert(OrderRankedConnectionPretest(noSavedMetrics) == (QList<int>{1, 2, 3}));
    assert(OrderRankedByConnectionTime(noSavedMetrics) == (QList<int>{1, 2, 3}));

    // Speedtest by connection time, first run (nothing saved):
    // 1) TTFB pretest walks selection order.
    // 2) Drop failures (connectTimeMs <= 0).
    // 3) Sort survivors by rising connect time.
    // 4) 2 MiB download runs in that order — shortest TTFB first.
    const QList<RankedScheduleRow> firstRunSelection = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
    assert(OrderRankedConnectionPretest(firstRunSelection) == (QList<int>{1, 2, 3, 4}));
    const QList<RankedScheduleRow> afterFirstPretest = {
        {1, 180, 0}, // slow
        {2, 40, 0},  // fastest
        {3, 0, 0},   // failed TTFB — must not be downloaded
        {4, 90, 0},  // mid
    };
    QList<RankedScheduleRow> firstRunSurvivors;
    for (const auto& row : afterFirstPretest) {
        if (row.connectTimeMs > 0) firstRunSurvivors.append(row);
    }
    assert(OrderRankedByConnectionTime(firstRunSurvivors) == (QList<int>{2, 4, 1}));
    assert(!OrderRankedByConnectionTime(firstRunSurvivors).contains(3));

    // Speedtest by connection time, second run (times already saved):
    // Pretest order uses last-run times: fastest third, then unknowns, then
    // the slower known rows. New TTFB timeout is 3x the last-run fastest, so
    // a previously slow profile is cut off instead of waiting the full 3s.
    const QList<RankedScheduleRow> savedTimes = {
        {1, 180, 0},
        {2, 40, 0},
        {3, 90, 0},
        {4, 0, 0},
        {5, 0, 0},
    };
    assert(OrderRankedConnectionPretest(savedTimes) == (QList<int>{2, 4, 5, 3, 1}));
    qint64 lastRunFastest = 0;
    for (const auto& row : savedTimes) {
        if (row.connectTimeMs > 0 && (lastRunFastest == 0 || row.connectTimeMs < lastRunFastest)) {
            lastRunFastest = row.connectTimeMs;
        }
    }
    assert(lastRunFastest == 40);
    assert(RankedFallShortConnectionTimeoutMs(3000, lastRunFastest) == 120);

    // All known, no unknowns: pretest order is already rising connect time,
    // which is also the download order after a clean pretest.
    const QList<RankedScheduleRow> knownOnly = {{9, 80, 0}, {10, 20, 0}, {11, 50, 0}};
    assert(OrderRankedConnectionPretest(knownOnly) == (QList<int>{10, 11, 9}));
    assert(OrderRankedByConnectionTime(knownOnly) == (QList<int>{10, 11, 9}));

    // Speedtest by saved site score: no pretest; download highest last score first.
    const QList<RankedScheduleRow> scored = {
        {1, 90, 10},
        {2, 0, 0},
        {3, 30, 80},
        {4, 0, 55},
        {5, 50, 0},
    };
    assert(OrderRankedBySavedSiteScore(scored) == (QList<int>{3, 4, 1, 2, 5}));

    // Clearing before ordering would make both ranked modes a no-op, so the
    // runner must snapshot first.
    QList<RankedScheduleRow> cleared = scored;
    for (auto& row : cleared) {
        row.connectTimeMs = 0;
        row.siteScore = 0;
    }
    assert(OrderRankedBySavedSiteScore(cleared) == RankedScheduleIds(cleared));
    assert(OrderRankedConnectionPretest(cleared) == RankedScheduleIds(cleared));
    assert(OrderRankedByConnectionTime(QList<RankedScheduleRow>{{11, 0, 0}}) == (QList<int>{11}));

    // Download fall-short: 2x fastest finished download, and never slower than
    // 3x best TTFB. That is what lets thousands of rows finish in about a minute.
    assert(RankedFallShortConnectionTimeoutMs(3000, 0) == 3000);
    assert(RankedFallShortConnectionTimeoutMs(3000, 100) == 300);
    assert(RankedFallShortConnectionTimeoutMs(200, 100) == 200);
    assert(RankedFallShortDownloadTimeoutMs(5000, 0, 0) == 5000);
    assert(RankedFallShortDownloadTimeoutMs(5000, 400, 0) == 800);
    assert(RankedFallShortDownloadTimeoutMs(5000, 400, 100) == 300);
    assert(RankedFallShortDownloadTimeoutMs(250, 400, 100) == 250);
}
