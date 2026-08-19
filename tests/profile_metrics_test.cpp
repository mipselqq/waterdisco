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
}
