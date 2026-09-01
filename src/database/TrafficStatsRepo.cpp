#include "include/database/TrafficStatsRepo.h"

namespace Configs {

    TrafficStatsRepo::TrafficStatsRepo(Database& database) : db(database) {
        createTables();
    }

    std::string TrafficStatsRepo::bucketExpr(long long bucketSecs, long long utcOffsetSecs) {
        const std::string b = std::to_string(bucketSecs);
        const std::string off = std::to_string(utcOffsetSecs);
        // floor((bucket_start + off) / b) * b - off; the parens around off keep a negative (west-of-UTC) offset valid.
        return "((bucket_start + (" + off + ")) / " + b + ") * " + b + " - (" + off + ")";
    }

    void TrafficStatsRepo::createTables() const {
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS config_traffic_minute (
                bucket_start INTEGER NOT NULL,
                profile_id   INTEGER NOT NULL,
                up           INTEGER NOT NULL DEFAULT 0,
                down         INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (bucket_start, profile_id)
            )
        )");
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS config_traffic_hour (
                bucket_start INTEGER NOT NULL,
                profile_id   INTEGER NOT NULL,
                up           INTEGER NOT NULL DEFAULT 0,
                down         INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (bucket_start, profile_id)
            )
        )");
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS app_traffic_minute (
                bucket_start INTEGER NOT NULL,
                process_name TEXT NOT NULL,
                up           INTEGER NOT NULL DEFAULT 0,
                down         INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (bucket_start, process_name)
            )
        )");
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS app_traffic_hour (
                bucket_start INTEGER NOT NULL,
                process_name TEXT NOT NULL,
                up           INTEGER NOT NULL DEFAULT 0,
                down         INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (bucket_start, process_name)
            )
        )");
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS config_meta (
                profile_id     INTEGER PRIMARY KEY,
                name           TEXT,
                group_name     TEXT,
                type           TEXT,
                server_address TEXT,
                first_seen     INTEGER NOT NULL DEFAULT 0,
                last_seen      INTEGER NOT NULL DEFAULT 0
            )
        )");
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS app_meta (
                process_name TEXT PRIMARY KEY,
                last_path    TEXT,
                first_seen   INTEGER NOT NULL DEFAULT 0,
                last_seen    INTEGER NOT NULL DEFAULT 0
            )
        )");
    }

    void TrafficStatsRepo::UpsertConfigMinuteBatch(const QList<ConfigTrafficRow>& rows) {
        if (rows.isEmpty()) return;
        std::lock_guard<std::mutex> lk(mu);
        try {
            db.execThrow("BEGIN IMMEDIATE");
            for (const auto& r : rows) {
                db.execThrow(
                    "INSERT INTO config_traffic_minute (bucket_start, profile_id, up, down) "
                    "VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(bucket_start, profile_id) DO UPDATE SET "
                    "up = up + excluded.up, down = down + excluded.down",
                    r.bucket_start, r.profile_id, r.up, r.down);
            }
            db.execThrow("COMMIT");
        } catch (std::exception& e) {
            try { db.execThrow("ROLLBACK"); } catch (...) {}
            NotifyError("UpsertConfigMinuteBatch", e);
        }
    }

    void TrafficStatsRepo::UpsertAppMinuteBatch(const QList<AppTrafficRow>& rows) {
        if (rows.isEmpty()) return;
        std::lock_guard<std::mutex> lk(mu);
        try {
            db.execThrow("BEGIN IMMEDIATE");
            for (const auto& r : rows) {
                db.execThrow(
                    "INSERT INTO app_traffic_minute (bucket_start, process_name, up, down) "
                    "VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(bucket_start, process_name) DO UPDATE SET "
                    "up = up + excluded.up, down = down + excluded.down",
                    r.bucket_start, r.process_name.toStdString(), r.up, r.down);
            }
            db.execThrow("COMMIT");
        } catch (std::exception& e) {
            try { db.execThrow("ROLLBACK"); } catch (...) {}
            NotifyError("UpsertAppMinuteBatch", e);
        }
    }

    void TrafficStatsRepo::UpsertConfigMeta(const ConfigMetaRow& m) {
        std::lock_guard<std::mutex> lk(mu);
        // On conflict last_seen and the mutable fields refresh, but the original first_seen is deliberately kept.
        db.exec(
            "INSERT INTO config_meta "
            "(profile_id, name, group_name, type, server_address, first_seen, last_seen) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(profile_id) DO UPDATE SET "
            "name = excluded.name, group_name = excluded.group_name, type = excluded.type, "
            "server_address = excluded.server_address, last_seen = excluded.last_seen",
            m.profile_id, m.name.toStdString(), m.group_name.toStdString(), m.type.toStdString(),
            m.server_address.toStdString(), m.first_seen, m.last_seen);
    }

    void TrafficStatsRepo::UpsertAppMeta(const QString& processName, const QString& lastPath, long long nowSecs) {
        std::lock_guard<std::mutex> lk(mu);
        db.exec(
            "INSERT INTO app_meta (process_name, last_path, first_seen, last_seen) "
            "VALUES (?, ?, ?, ?) "
            "ON CONFLICT(process_name) DO UPDATE SET "
            "last_path = excluded.last_path, last_seen = excluded.last_seen",
            processName.toStdString(), lastPath.toStdString(), nowSecs, nowSecs);
    }

    void TrafficStatsRepo::RollupMinuteToHour(long long olderThanSecs) {
        std::lock_guard<std::mutex> lk(mu);
        try {
            db.execThrow("BEGIN IMMEDIATE");
            db.execThrow(
                "INSERT INTO config_traffic_hour (bucket_start, profile_id, up, down) "
                "SELECT (bucket_start / 3600) * 3600, profile_id, SUM(up), SUM(down) "
                "FROM config_traffic_minute WHERE bucket_start < ? "
                "GROUP BY (bucket_start / 3600) * 3600, profile_id "
                "ON CONFLICT(bucket_start, profile_id) DO UPDATE SET "
                "up = up + excluded.up, down = down + excluded.down",
                olderThanSecs);
            db.execThrow("DELETE FROM config_traffic_minute WHERE bucket_start < ?", olderThanSecs);
            db.execThrow(
                "INSERT INTO app_traffic_hour (bucket_start, process_name, up, down) "
                "SELECT (bucket_start / 3600) * 3600, process_name, SUM(up), SUM(down) "
                "FROM app_traffic_minute WHERE bucket_start < ? "
                "GROUP BY (bucket_start / 3600) * 3600, process_name "
                "ON CONFLICT(bucket_start, process_name) DO UPDATE SET "
                "up = up + excluded.up, down = down + excluded.down",
                olderThanSecs);
            db.execThrow("DELETE FROM app_traffic_minute WHERE bucket_start < ?", olderThanSecs);
            db.execThrow("COMMIT");
        } catch (std::exception& e) {
            try { db.execThrow("ROLLBACK"); } catch (...) {}
            NotifyError("RollupMinuteToHour", e);
        }
    }

    void TrafficStatsRepo::PruneHour(long long olderThanSecs) {
        std::lock_guard<std::mutex> lk(mu);
        try {
            db.execThrow("BEGIN IMMEDIATE");
            db.execThrow("DELETE FROM config_traffic_hour WHERE bucket_start < ?", olderThanSecs);
            db.execThrow("DELETE FROM app_traffic_hour WHERE bucket_start < ?", olderThanSecs);
            db.execThrow("COMMIT");
        } catch (std::exception& e) {
            try { db.execThrow("ROLLBACK"); } catch (...) {}
            NotifyError("PruneHour", e);
        }
    }

    QList<ConfigUsage> TrafficStatsRepo::QueryConfigUsage(long long fromSecs, long long toSecs) {
        std::lock_guard<std::mutex> lk(mu);
        QList<ConfigUsage> out;
        auto q = db.query(
            "SELECT profile_id, SUM(u), SUM(d) FROM ("
            "  SELECT profile_id, up AS u, down AS d FROM config_traffic_minute "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            "  UNION ALL "
            "  SELECT profile_id, up AS u, down AS d FROM config_traffic_hour "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            ") GROUP BY profile_id",
            fromSecs, toSecs, fromSecs, toSecs);
        if (!q) return out;
        while (q->executeStep()) {
            ConfigUsage u;
            u.profile_id = q->getColumn(0).getInt();
            u.up = q->getColumn(1).getInt64();
            u.down = q->getColumn(2).getInt64();
            out.append(u);
        }
        return out;
    }

    QList<AppUsage> TrafficStatsRepo::QueryAppUsage(long long fromSecs, long long toSecs) {
        std::lock_guard<std::mutex> lk(mu);
        QList<AppUsage> out;
        auto q = db.query(
            "SELECT process_name, SUM(u), SUM(d) FROM ("
            "  SELECT process_name, up AS u, down AS d FROM app_traffic_minute "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            "  UNION ALL "
            "  SELECT process_name, up AS u, down AS d FROM app_traffic_hour "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            ") GROUP BY process_name",
            fromSecs, toSecs, fromSecs, toSecs);
        if (!q) return out;
        while (q->executeStep()) {
            AppUsage u;
            u.process_name = QString::fromUtf8(q->getColumn(0).getText());
            u.up = q->getColumn(1).getInt64();
            u.down = q->getColumn(2).getInt64();
            out.append(u);
        }
        return out;
    }

    QList<TrafficSeriesPoint> TrafficStatsRepo::QueryConfigSeries(long long fromSecs, long long toSecs, long long bucketSecs, long long utcOffsetSecs) {
        std::lock_guard<std::mutex> lk(mu);
        QList<TrafficSeriesPoint> out;
        if (bucketSecs <= 0) return out;
        // bucketSecs/utcOffsetSecs are internal numbers, safe to inline; shifting before the floor-divide snaps each bucket to the local calendar boundary, subtracting back returns that boundary's epoch.
        const std::string bkt = bucketExpr(bucketSecs, utcOffsetSecs);
        auto q = db.query(
            "SELECT " + bkt + " AS bkt, SUM(u), SUM(d) FROM ("
            "  SELECT bucket_start, up AS u, down AS d FROM config_traffic_minute "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            "  UNION ALL "
            "  SELECT bucket_start, up AS u, down AS d FROM config_traffic_hour "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            ") GROUP BY bkt ORDER BY bkt",
            fromSecs, toSecs, fromSecs, toSecs);
        if (!q) return out;
        while (q->executeStep()) {
            TrafficSeriesPoint p;
            p.bucket_start = q->getColumn(0).getInt64();
            p.up = q->getColumn(1).getInt64();
            p.down = q->getColumn(2).getInt64();
            out.append(p);
        }
        return out;
    }

    QList<TrafficSeriesPoint> TrafficStatsRepo::QueryAppSeries(long long fromSecs, long long toSecs, long long bucketSecs, long long utcOffsetSecs) {
        std::lock_guard<std::mutex> lk(mu);
        QList<TrafficSeriesPoint> out;
        if (bucketSecs <= 0) return out;
        const std::string bkt = bucketExpr(bucketSecs, utcOffsetSecs);
        auto q = db.query(
            "SELECT " + bkt + " AS bkt, SUM(u), SUM(d) FROM ("
            "  SELECT bucket_start, up AS u, down AS d FROM app_traffic_minute "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            "  UNION ALL "
            "  SELECT bucket_start, up AS u, down AS d FROM app_traffic_hour "
            "    WHERE bucket_start >= ? AND bucket_start < ? "
            ") GROUP BY bkt ORDER BY bkt",
            fromSecs, toSecs, fromSecs, toSecs);
        if (!q) return out;
        while (q->executeStep()) {
            TrafficSeriesPoint p;
            p.bucket_start = q->getColumn(0).getInt64();
            p.up = q->getColumn(1).getInt64();
            p.down = q->getColumn(2).getInt64();
            out.append(p);
        }
        return out;
    }

    QList<ConfigMetaRow> TrafficStatsRepo::GetAllConfigMeta() {
        std::lock_guard<std::mutex> lk(mu);
        QList<ConfigMetaRow> out;
        auto q = db.query(
            "SELECT profile_id, name, group_name, type, server_address, first_seen, last_seen FROM config_meta");
        if (!q) return out;
        while (q->executeStep()) {
            ConfigMetaRow m;
            m.profile_id = q->getColumn(0).getInt();
            m.name = QString::fromUtf8(q->getColumn(1).getText());
            m.group_name = QString::fromUtf8(q->getColumn(2).getText());
            m.type = QString::fromUtf8(q->getColumn(3).getText());
            m.server_address = QString::fromUtf8(q->getColumn(4).getText());
            m.first_seen = q->getColumn(5).getInt64();
            m.last_seen = q->getColumn(6).getInt64();
            out.append(m);
        }
        return out;
    }

    QList<AppMetaRow> TrafficStatsRepo::GetAllAppMeta() {
        std::lock_guard<std::mutex> lk(mu);
        QList<AppMetaRow> out;
        auto q = db.query("SELECT process_name, last_path, first_seen, last_seen FROM app_meta");
        if (!q) return out;
        while (q->executeStep()) {
            AppMetaRow m;
            m.process_name = QString::fromUtf8(q->getColumn(0).getText());
            m.last_path = QString::fromUtf8(q->getColumn(1).getText());
            m.first_seen = q->getColumn(2).getInt64();
            m.last_seen = q->getColumn(3).getInt64();
            out.append(m);
        }
        return out;
    }
}
