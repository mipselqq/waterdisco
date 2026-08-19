#pragma once

#include <QString>

namespace Configs {
    enum class PerformanceTestStatus : int {
        Untested = 0,
        Success,
        Skipped,
        Error,
    };

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
}
