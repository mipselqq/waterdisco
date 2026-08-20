#include "include/database/entities/ProfileMetrics.h"

#include <algorithm>
#include <cmath>

#include <QRegularExpression>
#include <QtGlobal>

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

    QList<int> RankedScheduleIds(const QList<RankedScheduleRow>& rows) {
        QList<int> ids;
        ids.reserve(rows.size());
        for (const auto& row : rows) ids.append(row.id);
        return ids;
    }

    QList<int> OrderRankedBySavedSiteScore(const QList<RankedScheduleRow>& rows) {
        const QList<int> original = RankedScheduleIds(rows);
        if (rows.size() < 2) return original;
        const bool haveValues = std::any_of(rows.cbegin(), rows.cend(), [](const RankedScheduleRow& row) {
            return row.siteScore > 0;
        });
        if (!haveValues) return original;

        QList<RankedScheduleRow> ordered = rows;
        std::stable_sort(ordered.begin(), ordered.end(), [](const RankedScheduleRow& left,
                                                            const RankedScheduleRow& right) {
            const bool leftKnown = left.siteScore > 0;
            const bool rightKnown = right.siteScore > 0;
            if (leftKnown != rightKnown) return leftKnown;
            if (!leftKnown) return false;
            return left.siteScore > right.siteScore;
        });
        return RankedScheduleIds(ordered);
    }

    QList<int> OrderRankedByConnectionTime(const QList<RankedScheduleRow>& rows) {
        const QList<int> original = RankedScheduleIds(rows);
        if (rows.size() < 2) return original;
        const bool haveValues = std::any_of(rows.cbegin(), rows.cend(), [](const RankedScheduleRow& row) {
            return row.connectTimeMs > 0;
        });
        if (!haveValues) return original;

        QList<RankedScheduleRow> ordered = rows;
        std::stable_sort(ordered.begin(), ordered.end(), [](const RankedScheduleRow& left,
                                                            const RankedScheduleRow& right) {
            const bool leftKnown = left.connectTimeMs > 0;
            const bool rightKnown = right.connectTimeMs > 0;
            if (leftKnown != rightKnown) return leftKnown;
            if (!leftKnown) return false;
            return left.connectTimeMs < right.connectTimeMs;
        });
        return RankedScheduleIds(ordered);
    }

    QList<int> OrderRankedConnectionPretest(const QList<RankedScheduleRow>& rows) {
        const QList<int> original = RankedScheduleIds(rows);
        if (rows.size() < 2) return original;

        QList<RankedScheduleRow> known;
        QList<int> unknown;
        known.reserve(rows.size());
        unknown.reserve(rows.size());
        for (const auto& row : rows) {
            if (row.connectTimeMs > 0) known.append(row);
            else unknown.append(row.id);
        }
        if (known.isEmpty()) return original;

        std::stable_sort(known.begin(), known.end(), [](const RankedScheduleRow& left,
                                                        const RankedScheduleRow& right) {
            return left.connectTimeMs < right.connectTimeMs;
        });

        QList<int> ordered;
        ordered.reserve(rows.size());
        if (unknown.isEmpty()) {
            for (const auto& row : known) ordered.append(row.id);
            return ordered;
        }

        const int bestKnownCount = std::max(1, static_cast<int>(known.size() / 3));
        for (int i = 0; i < bestKnownCount; ++i) ordered.append(known[i].id);
        ordered.append(unknown);
        for (int i = bestKnownCount; i < known.size(); ++i) ordered.append(known[i].id);
        return ordered;
    }

    int RankedFallShortConnectionTimeoutMs(int configuredTimeoutMs, qint64 bestConnectionMs) {
        const int configured = std::max(1, configuredTimeoutMs);
        if (bestConnectionMs <= 0) return configured;
        return static_cast<int>(std::min<qint64>(configured, std::max<qint64>(1, bestConnectionMs * 3)));
    }

    int RankedFallShortDownloadTimeoutMs(int configuredTimeoutMs, qint64 bestDownloadElapsedMs,
                                         qint64 bestConnectionMs) {
        int timeout = std::max(1, configuredTimeoutMs);
        if (bestDownloadElapsedMs > 0) {
            timeout = static_cast<int>(std::min<qint64>(timeout, std::max<qint64>(1, bestDownloadElapsedMs * 2)));
        }
        if (bestConnectionMs > 0) {
            timeout = static_cast<int>(std::min<qint64>(timeout, std::max<qint64>(1, bestConnectionMs * 3)));
        }
        return timeout;
    }
}
