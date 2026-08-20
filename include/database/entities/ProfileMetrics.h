#pragma once

#include <QtGlobal>
#include <QList>
#include <QString>

namespace Configs {
    enum class PerformanceTestStatus : int {
        Untested = 0,
        Success,
        Skipped,
        Error,
    };

    // Volatile context for Error/Skipped in the Rx speed column; cleared with
    // performance results and not persisted to the database.
    struct PerformanceStatusDetail {
        QString reason;
        qint64 measuredMs = -1;
        double measuredMbps = -1.0;
        int thresholdMs = -1;
    };

    [[nodiscard]] QString FormatPerformanceStatusTooltip(PerformanceTestStatus status,
                                                         const PerformanceStatusDetail& detail);

    // Waterdisco's browsing-oriented score: receive speed contributes 75%,
    // while connection setup time contributes 25% with a piecewise penalty.
    [[nodiscard]] int CalculateSiteScore(int connectionTimeMs, double rxMbps);
    [[nodiscard]] double ParseSpeedMbps(const QString &displaySpeed);

    // Successful measurements always sort before non-results. The order of the
    // latter is stable and explicit instead of being encoded in magic scores.
    [[nodiscard]] int PerformanceStatusRank(PerformanceTestStatus status);

    [[nodiscard]] bool PerformanceComesBefore(PerformanceTestStatus leftStatus,
                                              double leftValue,
                                              PerformanceTestStatus rightStatus,
                                              double rightValue,
                                              bool descending);

    // Last-run values captured before a ranked sweep clears the rows.
    struct RankedScheduleRow {
        int id = 0;
        int connectTimeMs = 0;
        int siteScore = 0;
    };

    [[nodiscard]] QList<int> RankedScheduleIds(const QList<RankedScheduleRow>& rows);

    // Highest last site score first; unknown scores keep their relative order.
    [[nodiscard]] QList<int> OrderRankedBySavedSiteScore(const QList<RankedScheduleRow>& rows);

    // Fastest this-run (or last-run) connect time first.
    [[nodiscard]] QList<int> OrderRankedByConnectionTime(const QList<RankedScheduleRow>& rows);

    // Last-launch pretest order: fastest third of known times, then unknowns,
    // then the slower known rows. Falls back to the given order when nothing
    // was measured last time.
    [[nodiscard]] QList<int> OrderRankedConnectionPretest(const QList<RankedScheduleRow>& rows);

    // Fall-short caps: 3x best TTFB for connection probes, and
    // min(2x best download wall time, 3x best TTFB) for the 2 MiB download.
    [[nodiscard]] int RankedFallShortConnectionTimeoutMs(int configuredTimeoutMs, qint64 bestConnectionMs);
    [[nodiscard]] int RankedFallShortDownloadTimeoutMs(int configuredTimeoutMs, qint64 bestDownloadElapsedMs,
                                                       qint64 bestConnectionMs);
}
