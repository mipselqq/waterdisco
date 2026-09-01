#pragma once

#include "Database.h"
#include <QString>
#include <QList>
#include <mutex>
#include <string>

namespace Configs {
    // bucket_start is a unix epoch second aligned to its tier (minute = multiple of 60, hour = multiple of 3600).
    struct ConfigTrafficRow {
        long long bucket_start = 0;
        int profile_id = 0;
        long long up = 0;
        long long down = 0;
    };

    struct AppTrafficRow {
        long long bucket_start = 0;
        QString process_name;
        long long up = 0;
        long long down = 0;
    };

    struct ConfigUsage {
        int profile_id = 0;
        long long up = 0;
        long long down = 0;
    };

    struct AppUsage {
        QString process_name;
        long long up = 0;
        long long down = 0;
    };

    struct TrafficSeriesPoint {
        long long bucket_start = 0;
        long long up = 0;
        long long down = 0;
    };

    // Kept so deleted/renamed configs and moved apps still resolve in the dashboard.
    struct ConfigMetaRow {
        int profile_id = 0;
        QString name;
        QString group_name;
        QString type;
        QString server_address;
        long long first_seen = 0;
        long long last_seen = 0;
    };

    struct AppMetaRow {
        QString process_name;
        QString last_path;
        long long first_seen = 0;
        long long last_seen = 0;
    };

    // Every public method is serialized by `mu`, so one shared instance is safe to call from the looper, rollup and UI threads.
    class TrafficStatsRepo {
    public:
        explicit TrafficStatsRepo(Database& database);

        // Upsert-add: accumulates into the existing minute bucket.
        void UpsertConfigMinuteBatch(const QList<ConfigTrafficRow>& rows);
        void UpsertAppMinuteBatch(const QList<AppTrafficRow>& rows);

        void UpsertConfigMeta(const ConfigMetaRow& meta);
        void UpsertAppMeta(const QString& processName, const QString& lastPath, long long nowSecs);

        // Atomic per call, so a crash never double-counts.
        void RollupMinuteToHour(long long olderThanSecs);
        void PruneHour(long long olderThanSecs);

        // Reads sum across both tiers over [fromSecs, toSecs).
        QList<ConfigUsage> QueryConfigUsage(long long fromSecs, long long toSecs);
        QList<AppUsage> QueryAppUsage(long long fromSecs, long long toSecs);
        QList<ConfigMetaRow> GetAllConfigMeta();
        QList<AppMetaRow> GetAllAppMeta();

        // Empty buckets are omitted; utcOffsetSecs (east of UTC) shifts the UTC-aligned grouping so bucket_start is the local boundary epoch.
        QList<TrafficSeriesPoint> QueryConfigSeries(long long fromSecs, long long toSecs, long long bucketSecs, long long utcOffsetSecs);
        QList<TrafficSeriesPoint> QueryAppSeries(long long fromSecs, long long toSecs, long long bucketSecs, long long utcOffsetSecs);

    private:
        Database& db;
        std::mutex mu;

        void createTables() const;
        // A member, not a file-local helper: the unity build would collide the symbol with another TU's.
        static std::string bucketExpr(long long bucketSecs, long long utcOffsetSecs);
    };
}
