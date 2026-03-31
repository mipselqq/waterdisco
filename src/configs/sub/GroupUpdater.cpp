#include "include/database/entities/Profile.h"
#include "include/global/HTTPRequestHelper.hpp"

#include "include/configs/sub/GroupUpdater.hpp"
#include "include/configs/sub/clash.hpp"

#include <QInputDialog>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>

#include <future>

#include "include/configs/common/utils.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"

namespace Subscription {

    GroupUpdater *groupUpdater = new GroupUpdater;

    int JsonEndIdx(const QString &str, int begin) {
        int sz = str.length();
        int counter = 1;
        for (int i=begin+1;i<sz;i++) {
            if (str[i] == '{') counter++;
            if (str[i] == '}') counter--;
            if (counter==0) return i;
        }
        return -1;
    }

    QList<QString> Disect(const QString &str) {
        QList<QString> res = QList<QString>();
        int idx=0;
        int sz = str.size();
        while(idx < sz) {
            if (str[idx] == '\n') {
                idx++;
                continue;
            }
            if (str[idx] == '{') {
                int endIdx = JsonEndIdx(str, idx);
                if (endIdx == -1) return res;
                res.append(str.mid(idx, endIdx-idx + 1));
                idx = endIdx+1;
                continue;
            }
            int nlineIdx = str.indexOf('\n', idx);
            if (nlineIdx == -1) nlineIdx = sz;
            res.append(str.mid(idx, nlineIdx-idx));
            idx = nlineIdx+1;
        }
        return res;
    }

    int subParseWorkerCount(int itemCount) {
        if (itemCount <= 0) return 1;
        int workers = QThread::idealThreadCount();
        if (workers <= 0) workers = 4;
        workers = std::clamp(workers, 2, 8);
        return std::min(workers, itemCount);
    }

    SingBoxSubType getSingBoxSubType(const QJsonDocument &doc) {
        if (doc.isObject()) {
            auto obj = doc.object();
            bool hasInbound = obj.contains("inbounds");
            bool hasOutbound = obj.contains("outbounds") || obj.contains("endpoints");
            // if (hasInbound && hasOutbound) return SingBoxSubType::fullConfig;
            if (hasOutbound) return SingBoxSubType::outboundInJson;
            if (obj.contains("type")) return SingBoxSubType::outboundObject;
            return SingBoxSubType::invalid;
        }
        if (doc.isArray() && !doc.array().empty()) {
            auto arr = doc.array();
            auto firstRaw = arr.first();
            if (firstRaw.isObject()) {
                auto obj = firstRaw.toObject();
                if (obj.contains("type")) return SingBoxSubType::outboundJsonArray;
            }
            return SingBoxSubType::invalid;
        }
        return SingBoxSubType::invalid;
    }

    static QByteArray decodeSubscriptionPayload(const QString &rawInput)
    {
        auto input = rawInput.trimmed();
        if (input.isEmpty()) return {};

        if (auto decoded = DecodeB64IfValid(input); !decoded.isEmpty()) {
            return decoded;
        }

        // Some providers/users pass wrapped payload with spaces/newlines or visual markers.
        auto compact = input;
        compact.remove(QRegularExpression("[\\s\\u23CE]"));
        if (auto decoded = DecodeB64IfValid(compact); !decoded.isEmpty()) {
            return decoded;
        }

        if (auto decoded = DecodeB64IfValid(compact, QByteArray::Base64UrlEncoding); !decoded.isEmpty()) {
            return decoded;
        }

        // Last fallback: normalize url-safe alphabet and restore padding.
        auto normalized = compact;
        normalized.replace('-', '+');
        normalized.replace('_', '/');
        auto rem = normalized.length() % 4;
        if (rem != 0) normalized += QString(4 - rem, '=');
        return DecodeB64IfValid(normalized);
    }

    void RawUpdater::update(const QString &str, bool needParse) {
        const QString line = str.trimmed();

        // Multi line
        if (line.count("\n") > 0 && needParse) {
            auto list = Disect(line);
            const int total = list.size();
            if (total == 0) return;

            // Large plain-link subscriptions (e.g. many vless:// lines) are CPU-bound on parsing.
            // Parse chunks in parallel and merge in original chunk order.
            if (total >= 300) {
                const int workers = subParseWorkerCount(total);
                const int chunkSize = (total + workers - 1) / workers;
                std::vector<std::future<QList<std::shared_ptr<Configs::Profile>>>> futures;
                futures.reserve(workers);

                for (int w = 0; w < workers; ++w) {
                    const int begin = w * chunkSize;
                    if (begin >= total) break;
                    const int end = std::min(total, begin + chunkSize);

                    futures.emplace_back(std::async(std::launch::async, [begin, end, &list]() {
                        RawUpdater localUpdater;
                        std::shared_ptr<QList<std::shared_ptr<Configs::Profile>>> parsed = std::make_shared<QList<std::shared_ptr<Configs::Profile>>>();
                        parsed->reserve(end - begin);

                        for (int i = begin; i < end; ++i) {
                            localUpdater.update(list.at(i).trimmed(), false);
                            if (!localUpdater.updated_order.isEmpty()) {
                                parsed->append(localUpdater.updated_order);
                                localUpdater.updated_order.clear();
                            }
                        }
                        return *parsed;
                    }));
                }

                for (auto &future : futures) {
                    auto parsed = future.get();
                    if (!parsed.isEmpty()) {
                        updated_order.append(parsed);
                    }
                }
                return;
            }

            for (const auto &str2: list) {
                update(str2.trimmed(), false);
            }
            return;
        }

        // is comment or too short
        if (line.startsWith("//") || line.startsWith("#") || line.length() < 2) {
            return;
        }

        // Base64 encoded subscription
        auto parseDirectLinkFast = [&](const QString& s) -> bool {
            std::shared_ptr<Configs::Profile> fastEnt;

            if (s.startsWith("json://")) {
                auto link = QUrl(s);
                if (!link.isValid()) return true;
                auto dataBytes = DecodeB64IfValid(link.fragment().toUtf8(), QByteArray::Base64UrlEncoding);
                if (dataBytes.isEmpty()) return true;
                auto data = QJsonDocument::fromJson(dataBytes).object();
                if (data.isEmpty()) return true;
                if (data.contains("protocol")) {
                    fastEnt = Configs::ProfilesRepo::NewProfile("xray" + data["protocol"].toString());
                } else {
                    fastEnt = data["type"].toString() == "hysteria2" ? Configs::ProfilesRepo::NewProfile("hysteria") : Configs::ProfilesRepo::NewProfile(data["type"].toString());
                }
                if (fastEnt->outbound->invalid) return true;
                fastEnt->outbound->ParseFromJson(data);
            } else if (s.startsWith("socks5://") || s.startsWith("socks4://") ||
                       s.startsWith("socks4a://") || s.startsWith("socks://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("socks");
                if (!fastEnt->Socks()->ParseFromLink(s)) return true;
            } else if (s.startsWith("http://") || s.startsWith("https://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("http");
                if (!fastEnt->Http()->ParseFromLink(s)) return true;
            } else if (s.startsWith("ss://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("shadowsocks");
                if (!fastEnt->ShadowSocks()->ParseFromLink(s)) return true;
            } else if (s.startsWith("vmess://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("vmess");
                if (!fastEnt->VMess()->ParseFromLink(s)) return true;
            } else if (s.startsWith("vless://")) {
                if (Configs::useXrayVless(s)) {
                    fastEnt = Configs::ProfilesRepo::NewProfile("xrayvless");
                    if (!fastEnt->XrayVLESS()->ParseFromLink(s)) return true;
                } else {
                    fastEnt = Configs::ProfilesRepo::NewProfile("vless");
                    if (!fastEnt->VLESS()->ParseFromLink(s)) return true;
                }
            } else if (s.startsWith("trojan://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("trojan");
                if (!fastEnt->Trojan()->ParseFromLink(s)) return true;
            } else if (s.startsWith("anytls://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("anytls");
                if (!fastEnt->AnyTLS()->ParseFromLink(s)) return true;
            } else if (s.startsWith("hysteria://") || s.startsWith("hysteria2://") || s.startsWith("hy2://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("hysteria");
                if (!fastEnt->Hysteria()->ParseFromLink(s)) return true;
            } else if (s.startsWith("tuic://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("tuic");
                if (!fastEnt->TUIC()->ParseFromLink(s)) return true;
            } else if (s.startsWith("juicity://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("juicity");
                if (!fastEnt->Juicity()->ParseFromLink(s)) return true;
            } else if (s.startsWith("tt://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("trusttunnel");
                if (!fastEnt->TrustTunnel()->ParseFromLink(s)) return true;
            } else if (s.startsWith("shadowtls://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("shadowtls");
                if (!fastEnt->ShadowTLS()->ParseFromLink(s)) return true;
            } else if (s.startsWith("wg://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("wireguard");
                if (!fastEnt->Wireguard()->ParseFromLink(s)) return true;
            } else if (s.startsWith("ssh://")) {
                fastEnt = Configs::ProfilesRepo::NewProfile("ssh");
                if (!fastEnt->SSH()->ParseFromLink(s)) return true;
            } else if (Configs::HasNaive() && (s.startsWith("naive+https://") || s.startsWith("naive+quic://"))) {
                fastEnt = Configs::ProfilesRepo::NewProfile("naive");
                if (!fastEnt->Naive()->ParseFromLink(s)) return true;
            } else {
                return false;
            }

            if (fastEnt != nullptr) updated_order += fastEnt;
            return true;
        };

        // Fast path for direct links in line-by-line mode (hot path for large plain subscriptions).
        if (!needParse && parseDirectLinkFast(line)) {
            return;
        }

        if (auto str2 = decodeSubscriptionPayload(line); !str2.isEmpty()) {
            update(str2);
            return;
        }

        std::shared_ptr<Configs::Profile> ent;

        // Json
        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(line.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError) {
            // SingBox
            auto subType = getSingBoxSubType(doc);
            if (subType == SingBoxSubType::fullConfig) {
                ent = Configs::ProfilesRepo::NewProfile("custom");
                ent->Custom()->type = "fullconfig";
                ent->Custom()->config = line;
                updated_order += ent;
            } else if (subType == SingBoxSubType::outboundObject) {
                ent = Configs::ProfilesRepo::NewProfile("custom");
                ent->Custom()->type = "outbound";
                ent->Custom()->config = line;
                updated_order += ent;
            } else if (subType == SingBoxSubType::outboundInJson || subType == SingBoxSubType::outboundJsonArray) {
                updateSingBox(doc, subType);
                return;
            }

            // SIP008
            if (line.contains("version") && line.contains("servers"))
            {
                updateSIP008(line);
                return;
            }

            return;
        }

        // Clash
        if (line.contains("proxies:")) {
            updateClash(line);
            return;
        }

        // Wireguard Config
        if (line.contains("[Interface]") && line.contains("[Peer]"))
        {
            updateWireguardFileConfig(line);
            return;
        }

        // Json base64 link format
        if (line.startsWith("json://")) {
            auto link = QUrl(line);
            if (!link.isValid()) return;
            auto dataBytes = DecodeB64IfValid(link.fragment().toUtf8(), QByteArray::Base64UrlEncoding);
            if (dataBytes.isEmpty()) return;
            auto data = QJsonDocument::fromJson(dataBytes).object();
            if (data.isEmpty()) return;
            if (data.contains("protocol")) {
                ent = Configs::ProfilesRepo::NewProfile("xray" + data["protocol"].toString());
            } else {
                ent = data["type"].toString() == "hysteria2" ? Configs::ProfilesRepo::NewProfile("hysteria") : Configs::ProfilesRepo::NewProfile(data["type"].toString());
            }
            if (ent->outbound->invalid) return;
            ent->outbound->ParseFromJson(data);
        }

        // Json
        if (line.startsWith('{')) {
            ent = Configs::ProfilesRepo::NewProfile("custom");
            auto custom = ent->Custom();
            auto obj = QString2QJsonObject(line);
            if (obj.contains("outbounds")) {
                custom->type = "fullconfig";
                custom->config = line;
            } else if (obj.contains("server")) {
                custom->type = "outbound";
                custom->config = line;
            } else {
                return;
            }
        }

        // SOCKS
        if (line.startsWith("socks5://") || line.startsWith("socks4://") ||
            line.startsWith("socks4a://") || line.startsWith("socks://")) {
            ent = Configs::ProfilesRepo::NewProfile("socks");
            auto ok = ent->Socks()->ParseFromLink(line);
            if (!ok) return;
        }

        // HTTP
        if (line.startsWith("http://") || line.startsWith("https://")) {
            ent = Configs::ProfilesRepo::NewProfile("http");
            auto ok = ent->Http()->ParseFromLink(line);
            if (!ok) return;
        }

        // ShadowSocks
        if (line.startsWith("ss://")) {
            ent = Configs::ProfilesRepo::NewProfile("shadowsocks");
            auto ok = ent->ShadowSocks()->ParseFromLink(line);
            if (!ok) return;
        }

        // VMess
        if (line.startsWith("vmess://")) {
            ent = Configs::ProfilesRepo::NewProfile("vmess");
            auto ok = ent->VMess()->ParseFromLink(line);
            if (!ok) return;
        }

        // VLESS
        if (line.startsWith("vless://")) {
            if (Configs::useXrayVless(line)) {
                ent = Configs::ProfilesRepo::NewProfile("xrayvless");
                auto ok = ent->XrayVLESS()->ParseFromLink(line);
                if (!ok) return;
            } else {
                ent = Configs::ProfilesRepo::NewProfile("vless");
                auto ok = ent->VLESS()->ParseFromLink(line);
                if (!ok) return;
            }
        }

        // Trojan
        if (line.startsWith("trojan://")) {
            ent = Configs::ProfilesRepo::NewProfile("trojan");
            auto ok = ent->Trojan()->ParseFromLink(line);
            if (!ok) return;
        }

        // AnyTLS
        if (line.startsWith("anytls://")) {
            ent = Configs::ProfilesRepo::NewProfile("anytls");
            auto ok = ent->AnyTLS()->ParseFromLink(line);
            if (!ok) return;
        }

        // Hysteria
        if (line.startsWith("hysteria://") || line.startsWith("hysteria2://") || line.startsWith("hy2://")) {
            ent = Configs::ProfilesRepo::NewProfile("hysteria");
            auto ok = ent->Hysteria()->ParseFromLink(line);
            if (!ok) return;
        }

        // TUIC
        if (line.startsWith("tuic://")) {
            ent = Configs::ProfilesRepo::NewProfile("tuic");
            auto ok = ent->TUIC()->ParseFromLink(line);
            if (!ok) return;
        }

        // Juicity
        if (line.startsWith("juicity://")) {
            ent = Configs::ProfilesRepo::NewProfile("juicity");
            auto ok = ent->Juicity()->ParseFromLink(line);
            if (!ok) return;
        }

        // TrustTunnel
        if (line.startsWith("tt://")) {
            ent = Configs::ProfilesRepo::NewProfile("trusttunnel");
            auto ok = ent->TrustTunnel()->ParseFromLink(line);
            if (!ok) return;
        }

        // ShadowTLS
        if (line.startsWith("shadowtls://")) {
            ent = Configs::ProfilesRepo::NewProfile("shadowtls");
            auto ok = ent->ShadowTLS()->ParseFromLink(line);
            if (!ok) return;
        }

        // Wireguard
        if (line.startsWith("wg://")) {
            ent = Configs::ProfilesRepo::NewProfile("wireguard");
            auto ok = ent->Wireguard()->ParseFromLink(line);
            if (!ok) return;
        }

        // SSH
        if (line.startsWith("ssh://")) {
            ent = Configs::ProfilesRepo::NewProfile("ssh");
            auto ok = ent->SSH()->ParseFromLink(line);
            if (!ok) return;
        }

        // Naive
        if (Configs::HasNaive() && (line.startsWith("naive+https://") || line.startsWith("naive+quic://"))) {
            ent = Configs::ProfilesRepo::NewProfile("naive");
            auto ok = ent->Naive()->ParseFromLink(line);
            if (!ok) return;
        }

        if (ent == nullptr) return;

        // End
        updated_order += ent;
    }

    void RawUpdater::updateSingBox(const QJsonDocument &doc, SingBoxSubType type)
    {
        QJsonArray outbounds, endpoints;
        if (type == SingBoxSubType::outboundInJson) {
            auto json = doc.object();
            outbounds = json["outbounds"].toArray();
            endpoints = json["endpoints"].toArray();
        } else if (type == SingBoxSubType::outboundJsonArray) {
            outbounds = doc.array();
        } else {
            return;
        }
        QJsonArray items;
        for (const auto& outbound : outbounds)
        {
            if (!outbound.isObject()) continue;
            items.append(outbound.toObject());
        }
        for (const auto& endpoint : endpoints)
        {
            if (!endpoint.isObject()) continue;
            items.append(endpoint.toObject());
        }

        for (const auto& o : items)
        {
            auto out = o.toObject();
            if (out.isEmpty())
            {
                MW_show_log("invalid outbound of type: " + o.type());
                continue;
            }

            std::shared_ptr<Configs::Profile> ent;

            // SOCKS
            if (out["type"] == "socks") {
                ent = Configs::ProfilesRepo::NewProfile("socks");
                auto ok = ent->Socks()->ParseFromJson(out);
                if (!ok) continue;
            }

            // HTTP
            if (out["type"] == "http") {
                ent = Configs::ProfilesRepo::NewProfile("http");
                auto ok = ent->Http()->ParseFromJson(out);
                if (!ok) continue;
            }

            // ShadowSocks
            if (out["type"] == "shadowsocks") {
                ent = Configs::ProfilesRepo::NewProfile("shadowsocks");
                auto ok = ent->ShadowSocks()->ParseFromJson(out);
                if (!ok) continue;
            }

            // VMess
            if (out["type"] == "vmess") {
                ent = Configs::ProfilesRepo::NewProfile("vmess");
                auto ok = ent->VMess()->ParseFromJson(out);
                if (!ok) continue;
            }

            // VLESS
            if (out["type"] == "vless") {
                ent = Configs::ProfilesRepo::NewProfile("vless");
                auto ok = ent->VLESS()->ParseFromJson(out);
                if (!ok) continue;
            }

            // Trojan
            if (out["type"] == "trojan") {
                ent = Configs::ProfilesRepo::NewProfile("trojan");
                auto ok = ent->Trojan()->ParseFromJson(out);
                if (!ok) continue;
            }

            // AnyTLS
            if (out["type"] == "anytls") {
                ent = Configs::ProfilesRepo::NewProfile("anytls");
                auto ok = ent->AnyTLS()->ParseFromJson(out);
                if (!ok) continue;
            }

            // Hysteria
            if (out["type"] == "hysteria" || out["type"] == "hysteria2") {
                ent = Configs::ProfilesRepo::NewProfile("hysteria");
                auto ok = ent->Hysteria()->ParseFromJson(out);
                if (!ok) continue;
            }

            // TUIC
            if (out["type"] == "tuic") {
                ent = Configs::ProfilesRepo::NewProfile("tuic");
                auto ok = ent->TUIC()->ParseFromJson(out);
                if (!ok) continue;
            }

            // Juicity
            if (out["type"] == "juicity") {
                ent = Configs::ProfilesRepo::NewProfile("juicity");
                auto ok = ent->Juicity()->ParseFromJson(out);
                if (!ok) continue;
            }

            // TrustTunnel
            if (out["type"] == "trusttunnel") {
                ent = Configs::ProfilesRepo::NewProfile("trusttunnel");
                auto ok = ent->TrustTunnel()->ParseFromJson(out);
                if (!ok) continue;
            }

            // ShadowTLS
            if (out["type"] == "shadowtls") {
                ent = Configs::ProfilesRepo::NewProfile("shadowtls");
                auto ok = ent->ShadowTLS()->ParseFromJson(out);
                if (!ok) continue;
            }

            // Wireguard
            if (out["type"] == "wireguard") {
                ent = Configs::ProfilesRepo::NewProfile("wireguard");
                auto ok = ent->Wireguard()->ParseFromJson(out);
                if (!ok) continue;
            }

            // SSH
            if (out["type"] == "ssh") {
                ent = Configs::ProfilesRepo::NewProfile("ssh");
                auto ok = ent->SSH()->ParseFromJson(out);
                if (!ok) continue;
            }

            // Naive
            if (Configs::HasNaive() && out["type"] == "naive") {
                ent = Configs::ProfilesRepo::NewProfile("naive");
                auto ok = ent->Naive()->ParseFromJson(out);
                if (!ok) continue;
            }

            if (ent == nullptr) continue;

            updated_order += ent;
        }
    }

    void RawUpdater::updateClash(const QString& str)
    {
        try {
            fkyaml::node node = fkyaml::node::deserialize(str.toStdString());
            clash::Clash clash_config = node.get_value<clash::Clash>();
    
            for (const auto& out : clash_config.proxies)
            {
                std::shared_ptr<Configs::Profile> ent;
    
                // SOCKS
                if (out.type == "socks5") {
                    ent = Configs::ProfilesRepo::NewProfile("socks");
                    auto ok = ent->Socks()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // HTTP
                if (out.type == "http") {
                    ent = Configs::ProfilesRepo::NewProfile("http");
                    auto ok = ent->Http()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // ShadowSocks
                if (out.type == "ss") {
                    ent = Configs::ProfilesRepo::NewProfile("shadowsocks");
                    auto ok = ent->ShadowSocks()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // VMess
                if (out.type == "vmess") {
                    ent = Configs::ProfilesRepo::NewProfile("vmess");
                    auto ok = ent->VMess()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // VLESS
                if (out.type == "vless") {
                    if (!out.encryption.empty() && out.encryption != "none") {
                        ent = Configs::ProfilesRepo::NewProfile("xrayvless");
                        auto ok = ent->XrayVLESS()->ParseFromClash(out);
                        if (!ok) continue;
                    } else {
                        ent = Configs::ProfilesRepo::NewProfile("vless");
                        auto ok = ent->VLESS()->ParseFromClash(out);
                        if (!ok) continue;
                    }
                }
    
                // Trojan
                if (out.type == "trojan") {
                    ent = Configs::ProfilesRepo::NewProfile("trojan");
                    auto ok = ent->Trojan()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // AnyTLS
                if (out.type == "anytls") {
                    ent = Configs::ProfilesRepo::NewProfile("anytls");
                    auto ok = ent->AnyTLS()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // Hysteria
                if (out.type == "hysteria" || out.type == "hysteria2") {
                    ent = Configs::ProfilesRepo::NewProfile("hysteria");
                    auto ok = ent->Hysteria()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // TUIC
                if (out.type == "tuic") {
                    ent = Configs::ProfilesRepo::NewProfile("tuic");
                    auto ok = ent->TUIC()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                // SSH
                if (out.type == "ssh") {
                    ent = Configs::ProfilesRepo::NewProfile("ssh");
                    auto ok = ent->SSH()->ParseFromClash(out);
                    if (!ok) continue;
                }
    
                if (ent == nullptr) continue;
    
                updated_order += ent;
            }
        } catch (const fkyaml::exception &ex) {
            runOnUiThread([=] {
                MessageBoxWarning("YAML Exception", ex.what());
            });
        }
    }

    void RawUpdater::updateWireguardFileConfig(const QString& str)
    {
        auto ent = Configs::ProfilesRepo::NewProfile("wireguard");
        auto ok = ent->Wireguard()->ParseFromLink(str);
        if (!ok) return;
        updated_order += ent;
    }

    void RawUpdater::updateSIP008(const QString& str)
    {
        auto json = QString2QJsonObject(str);

        for (const auto& o : json["servers"].toArray())
        {
            auto out = o.toObject();
            if (out.isEmpty())
            {
                MW_show_log("invalid server object");
                continue;
            }

            auto ent = Configs::ProfilesRepo::NewProfile("shadowsocks");
            auto ok = ent->ShadowSocks()->ParseFromSIP008(out);
            if (!ok) continue;
            updated_order += ent;
        }
    }

    // 在新的 thread 运行
    void GroupUpdater::AsyncUpdate(const QString &str, int _sub_gid, const std::function<void()> &finish) {
        auto content = str.trimmed();
        bool asURL = false;
        bool createNewGroup = false;

        if (_sub_gid < 0 && (content.startsWith("http://") || content.startsWith("https://"))) {
            auto items = QStringList{
                QObject::tr("Add profiles to this group"),
                QObject::tr("Create new subscription group"),
                QObject::tr("Import HTTP proxy profile"),
            };
            bool ok;
            auto a = QInputDialog::getItem(nullptr,
                                           QObject::tr("url detected"),
                                           QObject::tr("%1\nHow to update?").arg(content),
                                           items, 0, false, &ok);
            if (!ok) return;
            switch (items.indexOf(a)) {
                case 1: createNewGroup = true;
                case 0: asURL = true; break;
            }
        }

        runOnNewThread([=,this] {
            auto gid = _sub_gid;
            if (createNewGroup) {
                auto group = Configs::GroupsRepo::NewGroup();
                group->name = QUrl(str).host();
                group->url = str;
                Configs::dataManager->groupsRepo->AddGroup(group);
                gid = group->id;
                MW_dialog_message("SubUpdater", "NewGroup");
            }
            Update(str, gid, asURL);
            emit asyncUpdateCallback(gid);
            if (finish != nullptr) finish();
        });
    }

    void GroupUpdater::Update(const QString &_str, int _sub_gid, bool _not_sub_as_url) {
        // 创建 rawUpdater
        Configs::dataManager->settingsRepo->imported_count = 0;
        auto rawUpdater = std::make_unique<RawUpdater>();
        rawUpdater->gid_add_to = _sub_gid;

        // 准备
        QString sub_user_info;
        bool asURL = _sub_gid >= 0 || _not_sub_as_url; // 把 _str 当作 url 处理（下载内容）
        auto content = _str.trimmed();
        auto group = Configs::dataManager->groupsRepo->GetGroup(_sub_gid);
        if (group != nullptr && group->archive) return;

        // 网络请求
        if (asURL) {
            auto groupName = group == nullptr ? content : group->name;
            MW_show_log(">>>>>>>> " + QObject::tr("Requesting subscription: %1").arg(groupName));

            auto resp = NetworkRequestHelper::HttpGet(content, Configs::dataManager->settingsRepo->sub_send_hwid);
            if (!resp.error.isEmpty()) {
                MW_show_log("<<<<<<<< " + QObject::tr("Requesting subscription %1 error: %2").arg(groupName, resp.error + "\n" + resp.data));
                return;
            }

            content = resp.data;
            sub_user_info = NetworkRequestHelper::GetHeader(resp.header, "Subscription-UserInfo");

            MW_show_log("<<<<<<<< " + QObject::tr("Subscription request fininshed: %1").arg(groupName));
        }

        QList<std::shared_ptr<Configs::Profile>> in;

        if (group != nullptr) {
            group->sub_last_update = QDateTime::currentMSecsSinceEpoch() / 1000;
            group->info = sub_user_info;
            Configs::dataManager->groupsRepo->Save(group);
            //
            if (Configs::dataManager->settingsRepo->sub_clear) {
                MW_show_log(QObject::tr("Clearing servers..."));
                if (!Configs::dataManager->profilesRepo->BatchDeleteProfiles(group->Profiles())) {
                    runOnUiThread([=] {
                        MessageBoxWarning("Internal Error", "DB Error when deleting profiles, Please try again.");
                    });
                    return;
                }
            } else {
                in = Configs::dataManager->profilesRepo->GetProfileBatch(group->Profiles());
            }
        }

        MW_show_log(">>>>>>>> " + QObject::tr("Processing subscription data..."));
        rawUpdater->update(content);
        content.clear();
        Configs::dataManager->profilesRepo->AddProfileBatch(rawUpdater->updated_order, rawUpdater->gid_add_to);
        MW_show_log(">>>>>>>> " + QObject::tr("Process complete, applying..."));

        if (group != nullptr) {
            QList<std::shared_ptr<Configs::Profile>> out_all;
            out_all = Configs::dataManager->profilesRepo->GetProfileBatch(group->Profiles());;

            QString change_text;

            if (Configs::dataManager->settingsRepo->sub_clear) {
                // all is new profile
                if (out_all.size() >= 1000) {
                    change_text += "[+] " + Int2String(out_all.size()) + " profiles\n";
                } else {
                    for (const auto &ent: out_all) {
                        change_text += "[+] " + ent->outbound->DisplayTypeAndName() + "\n";
                    }
                }
            } else {
                QList<std::shared_ptr<Configs::Profile>> update_keep;
                QList<std::shared_ptr<Configs::Profile>> update_del;
                QList<std::shared_ptr<Configs::Profile>> only_out;
                QList<std::shared_ptr<Configs::Profile>> only_in;
                QList<std::shared_ptr<Configs::Profile>> out;
                // find and delete not updated profile by ProfileFilter
                Configs::ProfileFilter::OnlyInSrc_ByPointer(out_all, in, out);
                Configs::ProfileFilter::OnlyInSrc(in, out, only_in, false);
                Configs::ProfileFilter::OnlyInSrc(out, in, only_out, false);
                Configs::ProfileFilter::Common(in, out, update_keep, update_del, false);
                QString notice_added;
                QString notice_deleted;
                if (only_out.size() < 1000)
                {
                    for (const auto &ent: only_out) {
                        notice_added += "[+] " + ent->outbound->DisplayTypeAndName() + "\n";
                    }
                } else
                {
                    notice_added += QString("[+] ") + "added " + Int2String(only_out.size()) + "\n";
                }
                if (only_in.size() < 1000)
                {
                    for (const auto &ent: only_in) {
                        notice_deleted += "[-] " + ent->outbound->DisplayTypeAndName() + "\n";
                    }
                } else
                {
                    notice_deleted += QString("[-] ") + "deleted " + Int2String(only_in.size()) + "\n";
                }


                // sort according to order in remote
                group->profiles.clear();
                for (const auto &ent: rawUpdater->updated_order) {
                    auto deleted_index = update_del.indexOf(ent);
                    if (deleted_index >= 0) {
                        if (deleted_index >= update_keep.count()) continue; // should not happen
                        const auto& ent2 = update_keep[deleted_index];
                        group->profiles.append(ent2->id);
                    } else {
                        group->profiles.append(ent->id);
                    }
                }
                Configs::dataManager->groupsRepo->Save(group);

                // cleanup
                QList<int> del_ids;
                for (const auto &ent: out_all) {
                    if (!group->HasProfile(ent->id)) {
                        del_ids.append(ent->id);
                    }
                }
                if (!Configs::dataManager->profilesRepo->BatchDeleteProfiles(del_ids)) {
                    runOnUiThread([=] {
                       MessageBoxWarning("Internal error", "DB Error when deleting profiles, data may be corrupted");
                    });
                }

                change_text = "\n" + QObject::tr("Added %1 profiles:\n%2\nDeleted %3 Profiles:\n%4")
                                         .arg(only_out.length())
                                         .arg(notice_added)
                                         .arg(only_in.length())
                                         .arg(notice_deleted);
                if (only_out.length() + only_in.length() == 0) change_text = QObject::tr("Nothing");
            }

            MW_show_log("<<<<<<<< " + QObject::tr("Change of %1:").arg(group->name) + "\n" + change_text);
            MW_dialog_message("SubUpdater", "finish-dingyue");
        } else {
            Configs::dataManager->settingsRepo->imported_count = rawUpdater->updated_order.count();
            MW_dialog_message("SubUpdater", "finish");
        }
    }
} // namespace Subscription

bool UI_update_all_groups_Updating = false;

#define should_skip_group(g) (g == nullptr || g->url.isEmpty() || g->archive || (onlyAllowed && g->skip_auto_update))

void serialUpdateSubscription(const QList<int> &groupsTabOrder, int _order, bool onlyAllowed) {
    if (_order >= groupsTabOrder.size()) {
        UI_update_all_groups_Updating = false;
        return;
    }

    // calculate this group
    auto group = Configs::dataManager->groupsRepo->GetGroup(groupsTabOrder[_order]);
    if (group == nullptr || should_skip_group(group)) {
        serialUpdateSubscription(groupsTabOrder, _order + 1, onlyAllowed);
        return;
    }

    int nextOrder = _order + 1;
    while (nextOrder < groupsTabOrder.size()) {
        auto nextGid = groupsTabOrder[nextOrder];
        auto nextGroup = Configs::dataManager->groupsRepo->GetGroup(nextGid);
        if (!should_skip_group(nextGroup)) {
            break;
        }
        nextOrder += 1;
    }

    // Async update current group
    UI_update_all_groups_Updating = true;
    Subscription::groupUpdater->AsyncUpdate(group->url, group->id, [=] {
        serialUpdateSubscription(groupsTabOrder, nextOrder, onlyAllowed);
    });
}

void UI_update_all_groups(bool onlyAllowed) {
    if (UI_update_all_groups_Updating) {
        MW_show_log("The last subscription update has not exited.");
        return;
    }

    auto groupsTabOrder = Configs::dataManager->groupsRepo->GetGroupsTabOrder();
    serialUpdateSubscription(groupsTabOrder, 0, onlyAllowed);
}
