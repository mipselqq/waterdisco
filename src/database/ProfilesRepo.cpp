#include "include/database/ProfilesRepo.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "include/database/GroupsRepo.h"
#include "include/ui/mainwindow.h"
#include <QRegularExpression>


namespace Configs {
    namespace {
        double parseSpeedToMbps(QString speed) {
            speed = speed.trimmed();
            if (speed.isEmpty()) return 0.0;
            QRegularExpression re(R"(([-+]?\d*\.?\d+)\s*([kKmMgG])?bps)");
            auto m = re.match(speed);
            if (!m.hasMatch()) return 0.0;
            const double v = m.captured(1).toDouble();
            const QString u = m.captured(2).toLower();
            if (u == "g") return v * 1000.0;
            if (u == "m") return v;
            if (u == "k") return v / 1000.0;
            return v / 1000000.0;
        }
    }

    ProfilesRepo::ProfilesRepo(Database& database) : db(database) {
        createTables();
    }

    void ProfilesRepo::createTables() const {
        // Note: This table has a foreign key to groups(id).
        // Ensure GroupsRepo::createTables() is called before this method
        // to avoid foreign key constraint errors.
        // Create profiles table
        db.exec(R"(
            CREATE TABLE IF NOT EXISTS profiles (
                id INTEGER PRIMARY KEY,
                type TEXT NOT NULL,
                name TEXT,
                gid INTEGER NOT NULL DEFAULT 0,
                latency INTEGER NOT NULL DEFAULT 0,
                connect_time_ms INTEGER NOT NULL DEFAULT 0,
                site_score INTEGER NOT NULL DEFAULT -3,
                dl_speed TEXT,
                ul_speed TEXT,
                test_country TEXT,
                ip_out TEXT,
                outbound_json TEXT NOT NULL,
                traffic_dl INTEGER NOT NULL DEFAULT 0,
                traffic_up INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
                FOREIGN KEY(gid) REFERENCES groups(id) ON DELETE CASCADE
            )
        )");

        // Backward-compatible migration for existing databases.
        auto hasColumn = [this](const QString& columnName) {
            auto pragma = db.query("PRAGMA table_info(profiles)");
            if (!pragma) return false;
            while (pragma->executeStep()) {
                if (QString::fromStdString(pragma->getColumn(1).getText()) == columnName) {
                    return true;
                }
            }
            return false;
        };
        if (!hasColumn("connect_time_ms")) {
            db.exec("ALTER TABLE profiles ADD COLUMN connect_time_ms INTEGER NOT NULL DEFAULT 0");
        }
        if (!hasColumn("site_score")) {
            db.exec("ALTER TABLE profiles ADD COLUMN site_score INTEGER NOT NULL DEFAULT -3");
        }

        // Legacy normalization: old builds used 0 for "never tested".
        // Keep real measured zero scores intact (they have connect_time_ms > 0 or speed text).
        db.exec(R"(
            UPDATE profiles
            SET site_score = -3
            WHERE site_score = 0
              AND connect_time_ms = 0
              AND (dl_speed IS NULL OR TRIM(dl_speed) = '')
              AND (ul_speed IS NULL OR TRIM(ul_speed) = '')
        )");

        db.exec("CREATE INDEX IF NOT EXISTS idx_profiles_name ON profiles(name)");
    }

    QJsonObject ProfilesRepo::profileToJson(const Profile* profile) const {
        QJsonObject json;
        
        // Simple fields
        json["type"] = profile->type;
        json["name"] = profile->outbound->name;
        json["id"] = profile->id;
        json["gid"] = profile->gid;
        json["latency"] = profile->latency;
        json["connect_time_ms"] = profile->connect_time_ms;
        json["site_score"] = profile->site_score;
        json["dl_speed"] = profile->dl_speed;
        json["ul_speed"] = profile->ul_speed;
        json["test_country"] = "";
        json["ip_out"] = profile->ip_out;
        
        // Complex objects - serialize to JSON strings
        if (profile->outbound) {
            json["outbound"] = profile->outbound->ExportToJson();
        }
        
        json["traffic_dl"] = static_cast<qint64>(profile->traffic_downlink);
        json["traffic_up"] = static_cast<qint64>(profile->traffic_uplink);
        
        return json;
    }

    std::shared_ptr<Profile> ProfilesRepo::profileFromJson(const QJsonObject& json) const {
        auto profile = std::make_shared<Profile>();
        
        // Simple fields
        profile->type = json["type"].toString();
        profile->name = json["name"].toString();
        profile->id = json["id"].toInt();
        profile->gid = json["gid"].toInt();
        profile->latency = json["latency"].toInt();
        profile->connect_time_ms = json.contains("connect_time_ms") ? json["connect_time_ms"].toInt() : 0;
        profile->site_score = json.contains("site_score") ? json["site_score"].toInt() : -3;
        profile->dl_speed = json["dl_speed"].toString();
        profile->ul_speed = json["ul_speed"].toString();
        profile->dl_speed_mbps = parseSpeedToMbps(profile->dl_speed);
        profile->ul_speed_mbps = parseSpeedToMbps(profile->ul_speed);
        profile->ip_out = json["ip_out"].toString();
        
        // Reconstruct outbound (bean is not needed in new implementation)
        QString type = profile->type;
        if (type == "hysteria2") {
            type = "hysteria";
        }
        
        Configs::outbound* outbound = nullptr;
        
        // Create outbound based on type (bean is legacy, not needed)
        if (type == "socks") {
            outbound = new Configs::socks();
        } else if (type == "http") {
            outbound = new Configs::http();
        } else if (type == "shadowsocks") {
            outbound = new Configs::shadowsocks();
        } else if (type == "chain") {
            outbound = new Configs::chain();
        } else if (type == "vmess") {
            outbound = new Configs::vmess();
        } else if (type == "trojan") {
            outbound = new Configs::Trojan();
        } else if (type == "vless") {
            outbound = new Configs::vless();
        } else if (type == "xrayvless") {
            outbound = new Configs::xrayVless();
        } else if (type == "hysteria" || type == "hysteria2") {
            outbound = new Configs::hysteria();
        } else if (type == "tuic") {
            outbound = new Configs::tuic();
        } else if (type == "juicity") {
            outbound = new Configs::juicity();
        } else if (type == "trusttunnel") {
            outbound = new Configs::trusttunnel();
        } else if (type == "anytls") {
            outbound = new Configs::anyTLS();
        } else if (type == "shadowtls") {
            outbound = new Configs::shadowtls();
        } else if (type == "wireguard") {
            outbound = new Configs::wireguard();
        } else if (type == "tailscale") {
            outbound = new Configs::tailscale();
        } else if (type == "ssh") {
            outbound = new Configs::ssh();
        } else if (type == "custom") {
            outbound = new Configs::Custom();
        } else if (type == "extracore") {
            outbound = new Configs::extracore();
        } else if (Configs::HasNaive() && type == "naive") {
            outbound = new Configs::naive();
        } else {
            outbound = new Configs::outbound();
            outbound->invalid = true;
        }
        
        profile->outbound = std::shared_ptr<Configs::outbound>(outbound);
        
        // Parse complex objects from JSON
        if (json.contains("outbound") && json["outbound"].isObject()) {
            profile->outbound->ParseFromJson(json["outbound"].toObject());
        }
        
        if (json.contains("traffic_dl")) profile->traffic_downlink = json["traffic_dl"].toVariant().toLongLong();
        if (json.contains("traffic_up")) profile->traffic_uplink = json["traffic_up"].toVariant().toLongLong();
        
        profile->name = profile->outbound->name;
        
        return profile;
    }

    void ProfilesRepo::saveToDatabase(const Profile* profile, int id) const {
        QJsonObject json = profileToJson(profile);
        QJsonDocument doc(json);
        QString jsonStr = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        
        QString outboundJson;
        if (profile->outbound) {
            QJsonDocument outboundDoc(profile->outbound->ExportToJson());
            outboundJson = QString::fromUtf8(outboundDoc.toJson(QJsonDocument::Compact));
        }
        QString name = profile->outbound ? profile->outbound->name : QString();
        const long long traffic_dl = static_cast<long long>(profile->traffic_downlink);
        const long long traffic_up = static_cast<long long>(profile->traffic_uplink);
        
        auto checkQuery = db.query("SELECT id FROM profiles WHERE id = ?", id);
        bool exists = checkQuery && checkQuery->executeStep();
        
        if (exists) {
            db.exec(R"(
                UPDATE profiles 
                SET type = ?, name = ?, gid = ?, latency = ?, connect_time_ms = ?, site_score = ?, dl_speed = ?, ul_speed = ?, 
                    test_country = ?, ip_out = ?, outbound_json = ?,
                    traffic_dl = ?, traffic_up = ?, updated_at = strftime('%s', 'now')
                WHERE id = ?
            )", 
                profile->type.toStdString(),
                name.toStdString(),
                profile->gid,
                profile->latency,
                profile->connect_time_ms,
                profile->site_score,
                profile->dl_speed.toStdString(),
                profile->ul_speed.toStdString(),
                "",
                profile->ip_out.toStdString(),
                outboundJson.toStdString(),
                traffic_dl,
                traffic_up,
                id
            );
        } else {
            db.exec(R"(
                INSERT INTO profiles 
                (id, type, name, gid, latency, connect_time_ms, site_score, dl_speed, ul_speed, test_country, 
                ip_out, outbound_json, traffic_dl, traffic_up)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )",
                id,
                profile->type.toStdString(),
                name.toStdString(),
                profile->gid,
                profile->latency,
                profile->connect_time_ms,
                profile->site_score,
                profile->dl_speed.toStdString(),
                profile->ul_speed.toStdString(),
                "",
                profile->ip_out.toStdString(),
                outboundJson.toStdString(),
                traffic_dl,
                traffic_up
            );
        }
    }

    ProfileInsertRow ProfilesRepo::profileToInsertRow(const Profile* profile, int id, int gid) const {
        QString outboundJson;
        if (profile->outbound) {
            outboundJson = QString::fromUtf8(QJsonDocument(profile->outbound->ExportToJson()).toJson(QJsonDocument::Compact));
        }
        QString name = profile->outbound ? profile->outbound->name : QString();
        ProfileInsertRow row;
        row.id = id;
        row.type = profile->type.toStdString();
        row.name = name.toStdString();
        row.gid = gid;
        row.latency = profile->latency;
        row.connect_time_ms = profile->connect_time_ms;
        row.site_score = profile->site_score;
        row.dl_speed = profile->dl_speed.toStdString();
        row.ul_speed = profile->ul_speed.toStdString();
        row.test_country = "";
        row.ip_out = profile->ip_out.toStdString();
        row.outbound_json = outboundJson.toStdString();
        row.traffic_dl = static_cast<long long>(profile->traffic_downlink);
        row.traffic_up = static_cast<long long>(profile->traffic_uplink);
        return row;
    }

    std::shared_ptr<Profile> ProfilesRepo::profileFromRow(SQLite::Statement& stmt) const {
        QJsonObject json;
        json["id"] = stmt.getColumn(0).getInt();
        json["type"] = QString::fromStdString(stmt.getColumn(1).getText());
        json["name"] = QString::fromStdString(stmt.getColumn(2).getText());
        json["gid"] = stmt.getColumn(3).getInt();
        json["latency"] = stmt.getColumn(4).getInt();
        json["connect_time_ms"] = stmt.getColumn(5).getInt();
        json["site_score"] = stmt.getColumn(6).getInt();
        json["dl_speed"] = QString::fromStdString(stmt.getColumn(7).getText());
        json["ul_speed"] = QString::fromStdString(stmt.getColumn(8).getText());
        json["test_country"] = QString::fromStdString(stmt.getColumn(9).getText());
        json["ip_out"] = QString::fromStdString(stmt.getColumn(10).getText());
        
        QString outboundJsonStr = QString::fromStdString(stmt.getColumn(11).getText());
        QJsonDocument outboundDoc = QJsonDocument::fromJson(outboundJsonStr.toUtf8());
        if (!outboundDoc.isNull() && outboundDoc.isObject()) {
            json["outbound"] = outboundDoc.object();
        }
        
        json["traffic_dl"] = static_cast<qint64>(stmt.getColumn(12).getInt64());
        json["traffic_up"] = static_cast<qint64>(stmt.getColumn(13).getInt64());
        
        return profileFromJson(json);
    }

    std::shared_ptr<Profile> ProfilesRepo::loadFromDatabase(int id) const {
        auto query = db.query(R"(
            SELECT id, type, name, gid, latency, connect_time_ms, site_score, dl_speed, ul_speed, test_country, 
                   ip_out, outbound_json, traffic_dl, traffic_up
            FROM profiles WHERE id = ?
        )", id);
        if (!query || !query->executeStep()) {
            return nullptr;
        }
        return profileFromRow(*query);
    }

    std::shared_ptr<Profile> ProfilesRepo::NewProfile(const QString &type) {
        Configs::outbound *outbound = nullptr;
        
        // Create outbound based on type (bean is legacy, not needed)
        if (type == "socks") {
            outbound = new Configs::socks();
        } else if (type == "http") {
            outbound = new Configs::http();
        } else if (type == "shadowsocks") {
            outbound = new Configs::shadowsocks();
        } else if (type == "chain") {
            outbound = new Configs::chain();
        } else if (type == "vmess") {
            outbound = new Configs::vmess();
        } else if (type == "trojan") {
            outbound = new Configs::Trojan();
        } else if (type == "vless") {
            outbound = new Configs::vless();
        } else if (type == "xrayvless") {
            outbound = new Configs::xrayVless();
        } else if (type == "hysteria" || type == "hysteria2") {
            outbound = new Configs::hysteria();
        } else if (type == "tuic") {
            outbound = new Configs::tuic();
        } else if (type == "juicity") {
            outbound = new Configs::juicity();
        } else if (type == "trusttunnel") {
            outbound = new Configs::trusttunnel();
        } else if (type == "anytls") {
            outbound = new Configs::anyTLS();
        } else if (type == "shadowtls") {
            outbound = new Configs::shadowtls();
        } else if (type == "wireguard") {
            outbound = new Configs::wireguard();
        } else if (type == "tailscale") {
            outbound = new Configs::tailscale();
        } else if (type == "ssh") {
            outbound = new Configs::ssh();
        } else if (type == "custom") {
            outbound = new Configs::Custom();
        } else if (type == "extracore") {
            outbound = new Configs::extracore();
        } else if (Configs::HasNaive() && type == "naive") {
            outbound = new Configs::naive();
        } else {
            outbound = new Configs::outbound();
            outbound->invalid = true;
        }
        
        // Bean is legacy, pass nullptr
        return std::make_shared<Profile>(outbound, type);
    }

    bool ProfilesRepo::AddProfile(std::shared_ptr<Profile>& profile, int gid) {
        if (profile->id >= 0) return false;
        int newId = NewProfileID();
        profile->id = newId;
        profile->gid = gid < 0 ? Configs::dataManager->settingsRepo->current_group : gid;
        QMutexLocker locker(&mutex);
        identityMap[newId] = std::weak_ptr<Profile>(profile);
        saveToDatabase(profile.get(), profile->id);
        if (auto group = dataManager->groupsRepo->GetGroup(profile->gid)) {
            group->AddProfile(profile->id);
            dataManager->groupsRepo->Save(group);
        } else {
            return false;
        }
        return true;
    }

    bool ProfilesRepo::AddProfileBatch(QList<std::shared_ptr<Profile>>& profiles, int gid) {
        gid = gid < 0 ? Configs::dataManager->settingsRepo->current_group : gid;
        auto group = dataManager->groupsRepo->GetGroup(gid);
        if (!group) return false;

        QList<std::shared_ptr<Profile>> toAdd;
        for (auto& profile : profiles) {
            if (profile->id < 0) toAdd.append(profile);
        }
        if (toAdd.isEmpty()) return true;

        int n = toAdd.size();
        int firstId = NewProfileIDRange(n);

        QMutexLocker locker(&mutex);
        for (int i = 0; i < n; ++i) {
            int id = firstId + i;
            toAdd[i]->id = id;
            toAdd[i]->gid = gid;
            identityMap[id] = std::weak_ptr<Profile>(toAdd[i]);
        }

        std::vector<ProfileInsertRow> rows;
        rows.reserve(n);
        for (int i = 0; i < n; ++i) {
            rows.push_back(profileToInsertRow(toAdd[i].get(), toAdd[i]->id, toAdd[i]->gid));
        }
        db.execBatchInsertProfiles(rows);

        QList<int> profileIDs;
        for (const auto& profile : toAdd) {
            profileIDs << profile->id;
        }
        group->AddProfileBatch(profileIDs);
        dataManager->groupsRepo->Save(group);

        return true;
    }

    std::shared_ptr<Profile> ProfilesRepo::GetProfile(int id) const {
        QMutexLocker locker(&mutex);
        if (auto it = identityMap.find(id); it != identityMap.end()) {
            if (auto shared = it->second.lock()) return shared;
            identityMap.erase(it);
        }
        auto profile = loadFromDatabase(id);
        if (!profile) return nullptr;
        identityMap[id] = std::weak_ptr<Profile>(profile);
        return profile;
    }

    std::map<int, std::shared_ptr<Profile>> ProfilesRepo::loadProfilesByIdsChunk(const QList<int>& chunkIds) const {
        std::map<int, std::shared_ptr<Profile>> result;
        if (chunkIds.isEmpty()) return result;
        QString idList;
        for (int i = 0; i < chunkIds.size(); ++i) {
            if (i > 0) idList += ",";
            idList += QString::number(chunkIds[i]);
        }
        std::string sql = "SELECT id, type, name, gid, latency, connect_time_ms, site_score, dl_speed, ul_speed, test_country, "
                         "ip_out, outbound_json, traffic_dl, traffic_up FROM profiles WHERE id IN (" +
                         idList.toStdString() + ") ORDER BY id";
        auto query = db.query(sql);
        if (!query) return result;
        while (query->executeStep()) {
            auto profile = profileFromRow(*query);
            result[profile->id] = std::move(profile);
        }
        return result;
    }

    QList<std::shared_ptr<Profile>> ProfilesRepo::GetProfileBatch(QList<int> ids) {
        QList<std::shared_ptr<Profile>> profiles;
        if (ids.isEmpty()) return profiles;

        std::map<int, std::shared_ptr<Profile>> byId;
        QList<int> missingIds;
        QMutexLocker locker(&mutex);
        for (int id : ids) {
            auto it = identityMap.find(id);
            if (it != identityMap.end()) {
                if (auto shared = it->second.lock()) {
                    byId[id] = shared;
                    continue;
                }
                identityMap.erase(it);
            }
            missingIds.append(id);
        }
        if (missingIds.isEmpty()) {
            for (int id : ids) {
                auto it = byId.find(id);
                if (it != byId.end()) profiles.push_back(it->second);
            }
            return profiles;
        }

        for (int off = 0; off < missingIds.size(); off += Configs::BATCH_LIMIT_READ) {
            int end = std::min(off + Configs::BATCH_LIMIT_READ, static_cast<int>(missingIds.size()));
            auto chunk = missingIds.sliced(off, end - off);
            std::map<int, std::shared_ptr<Profile>> loaded = loadProfilesByIdsChunk(chunk);
            for (auto& p : loaded) byId[p.first] = std::move(p.second);
        }
        for (const auto& p : byId) {
            identityMap[p.first] = std::weak_ptr<Profile>(p.second);
        }
        for (int id : ids) {
            auto it = byId.find(id);
            if (it != byId.end()) profiles.push_back(it->second);
        }
        return profiles;
    }

    std::shared_ptr<Profile> ProfilesRepo::GetProfileByName(const QString& name) {
        // Query by name using the index
        auto query = db.query("SELECT id FROM profiles WHERE name = ? LIMIT 1", name.toStdString());
        if (!query || !query->executeStep()) {
            return nullptr;
        }
        
        int id = query->getColumn(0).getInt();
        return GetProfile(id);
    }

    QList<std::pair<int, QString> > ProfilesRepo::GetAllProfileIDNameMapped() {
        auto query = db.query("SELECT id, name FROM profiles ORDER BY id");
        if (!query) return {};
        QList<std::pair<int, QString> > res;
        while (query->executeStep()) {
            res.append({query->getColumn(0).getInt(), QString(query->getColumn(1).getString().c_str())});
        }
        return res;
    }

    QStringList ProfilesRepo::GetAllProfileNames() {
        auto query = db.query("SELECT name FROM profiles ORDER BY id");
        if (!query) return {};
        QStringList names;
        while (query->executeStep()) {
            names.append(QString(query->getColumn(0).getString().c_str()));
        }
        return names;
    }

    bool ProfilesRepo::BatchDeleteProfiles(const QList<int>& ids) {
        QSet<int> groupIDs;
        auto profiles = GetProfileBatch(ids);
        for (const auto& ent : profiles) {
            if (ent->id == dataManager->settingsRepo->started_id) {
                GetMainWindow()->profile_stop(false, true, false);
            }
            groupIDs.insert(ent->gid);
        }
        for (auto groupID : groupIDs) {
            auto group = dataManager->groupsRepo->GetGroup(groupID);
            if (!group) {
                MW_show_log("Could not find group with id " + Int2String(groupID));
                return false;
            }
            group->RemoveProfileBatch(ids);
            dataManager->groupsRepo->Save(group);
        }
        QMutexLocker locker(&mutex);
        for (int id : ids) identityMap.erase(id);
        if (!ids.isEmpty()) {
            std::vector<int> idVec(ids.begin(), ids.end());
            db.execDeleteByIdIn("profiles", "id", idVec);
        }
        return true;
    }

    QList<int> ProfilesRepo::GetAllProfileIds() const {
        QList<int> ids;
        auto query = db.query("SELECT id FROM profiles ORDER BY id");
        if (query) {
            while (query->executeStep()) {
                ids.append(query->getColumn(0).getInt());
            }
        }
        return ids;
    }

    int ProfilesRepo::NewProfileID() const {
        // Atomically increment and get the new ID using RETURNING clause (DB atomic, no lock required)
        auto query = db.query("UPDATE entity_ids SET profile_last_id = profile_last_id + 1 RETURNING profile_last_id");
        if (query && query->executeStep()) {
            return query->getColumn(0).getInt();
        }
        return 0;
    }

    int ProfilesRepo::NewProfileIDRange(int n) const {
        if (n <= 0) return 0;
        // Atomically reserve n IDs; RETURNING gives the new value (old + n), so first ID = newValue - n + 1
        auto query = db.query("UPDATE entity_ids SET profile_last_id = profile_last_id + ? RETURNING profile_last_id", n);
        if (query && query->executeStep()) {
            int newValue = query->getColumn(0).getInt();
            return newValue - n + 1;
        }
        return 0;
    }

    bool ProfilesRepo::Save(const std::shared_ptr<Profile>& profile) {
        if (!profile || profile->id < 0) {
            return false;
        }
        
        QMutexLocker locker(&mutex);
        saveToDatabase(profile.get(), profile->id);
        identityMap[profile->id] = std::weak_ptr<Profile>(profile);
        
        return true;
    }

    bool ProfilesRepo::SaveTraffic(const std::shared_ptr<Profile>& profile) {
        if (!profile || profile->id < 0) {
            return false;
        }
        const int id = profile->id;
        const long long dl = static_cast<long long>(profile->traffic_downlink);
        const long long up = static_cast<long long>(profile->traffic_uplink);
        runOnNewThread([=, this] {
            db.exec("UPDATE profiles SET traffic_dl = ?, traffic_up = ? WHERE id = ?", dl, up, id);
        });
        return true;
    }

    void ProfilesRepo::SaveBatch(const QList<std::shared_ptr<Profile>>& profiles) {
        runOnNewThread([=, this] {
            QList<std::shared_ptr<Profile>> valid;
            for (const auto& p : profiles) {
                if (p && p->id >= 0) valid.append(p);
            }
            if (valid.isEmpty()) return;
            std::vector<ProfileInsertRow> rows;
            rows.reserve(valid.size());
            for (const auto& p : valid) {
                rows.push_back(profileToInsertRow(p.get(), p->id, p->gid));
            }
            QMutexLocker locker(&mutex);
            db.execBatchReplaceProfiles(rows);
            for (const auto& p : valid) {
                identityMap[p->id] = std::weak_ptr<Profile>(p);
            }
        });
    }
}
