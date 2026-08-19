#include "include/database/entities/ProfileMetrics.h"

#include <algorithm>
#include <cmath>

#include <QRegularExpression>

namespace Configs {
    int CalculateSiteScore(int connectionTimeMs, double rxMbps) {
        if (connectionTimeMs <= 0 || rxMbps <= 0.0) return 0;

        const double ms = static_cast<double>(connectionTimeMs);
        double penalty;
        if (ms <= 120.0) {
            penalty = ms * 0.08;
        } else if (ms <= 250.0) {
            penalty = 120.0 * 0.08 + (ms - 120.0) * 0.18;
        } else {
            penalty = 120.0 * 0.08 + 130.0 * 0.18 + (ms - 250.0) * 0.38;
        }

        const double connectionScore = std::clamp(100.0 - penalty, 0.0, 100.0);
        const double rxScore = std::clamp(rxMbps * 0.92, 0.0, 100.0);
        return static_cast<int>(std::round(connectionScore * 0.25 + rxScore * 0.75));
    }

    double ParseSpeedMbps(const QString &displaySpeed) {
        const QRegularExpression pattern(
            QStringLiteral(R"(^\s*([-+]?\d*\.?\d+)\s*([kKmMgG])?bps\s*$)"));
        const auto match = pattern.match(displaySpeed);
        if (!match.hasMatch()) return 0.0;
        const double value = match.captured(1).toDouble();
        const QString unit = match.captured(2).toLower();
        if (unit == QStringLiteral("g")) return value * 1000.0;
        if (unit == QStringLiteral("m")) return value;
        if (unit == QStringLiteral("k")) return value / 1000.0;
        return value / 1000000.0;
    }

    int PerformanceStatusRank(PerformanceTestStatus status) {
        switch (status) {
        case PerformanceTestStatus::Success: return 0;
        case PerformanceTestStatus::Untested: return 1;
        case PerformanceTestStatus::Skipped: return 2;
        case PerformanceTestStatus::Error: return 3;
        }
        return 3;
    }

    bool PerformanceComesBefore(PerformanceTestStatus leftStatus, double leftValue,
                                PerformanceTestStatus rightStatus, double rightValue,
                                bool descending) {
        const int leftRank = PerformanceStatusRank(leftStatus);
        const int rightRank = PerformanceStatusRank(rightStatus);
        if (leftRank != rightRank) return leftRank < rightRank;
        if (leftRank != 0 || leftValue == rightValue) return false;
        return descending ? leftValue > rightValue : leftValue < rightValue;
    }
}
