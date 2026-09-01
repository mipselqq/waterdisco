#include "include/configs/common/utils.h"

#include "include/configs/common/TLS.h"
#include "include/configs/common/transport.h"
#include "include/global/Configs.hpp"
#include "include/global/Utils.hpp"

#include <QJsonArray>

namespace Configs
{
    void mergeUrlQuery(QUrlQuery& baseQuery, const QString& strQuery)
    {
        QUrlQuery query = QUrlQuery(strQuery);
        for (const auto& item : query.queryItems())
        {
            baseQuery.addQueryItem(item.first, item.second);
        }
    }

    void mergeJsonObjects(QJsonObject& baseObject, const QJsonObject& obj)
    {
        for (const auto& key : obj.keys())
        {
            baseObject[key] = obj[key];
        }
    }

    QStringList jsonObjectToQStringList(const QJsonObject& obj)
    {
        auto result = QStringList();
        for (const auto& key : obj.keys())
        {
            result << key << obj[key].toString();
        }
        return result;
    }

    QJsonObject qStringListToJsonObject(const QStringList& list)
    {
        auto result = QJsonObject();
        if (list.count() %2 != 0)
        {
            qDebug() << "QStringList of odd length in qStringListToJsonObject:" << list;
            return result;
        }
        for (int i=0;i<list.size();i+=2)
        {
            result[list[i]] = list[i+1];
        }
        return result;
    }

    // TODO add setting items and use them here
    bool useXrayVless(const QString& link) {
        auto url = QUrl(link);
        if (!url.isValid()) return false;
        auto query = QUrlQuery(url.query());
        const auto transport = query.queryItemValue("type");
        // sing-box has no equivalent of the raw HTTP header, so these must go to Xray.
        const bool rawHttp = (transport.isEmpty() || transport == "tcp" || transport == "raw")
                             && query.queryItemValue("headerType") == "http";

        if (dataManager->settingsRepo->xray_vless_preference == Xray::AllVLESS
            || query.queryItemValue("type") == "xhttp"
            || query.queryItemValue("type") == "kcp"
            || (query.queryItemValue("security") == "reality" && dataManager->settingsRepo->xray_vless_preference == Xray::XhttpAndReality)
            || (query.queryItemValue("encryption") != "none" && query.queryItemValue("encryption") != "")
            || query.queryItemValue("extra") != "") return true;
        return false;
    }

    bool singBoxTransportNeedsXray(const QString &type)
    {
        static const QStringList unsupported = {"kcp"};
        return unsupported.contains(type);
    }

    bool isCoreUnsupportedTransport(const QString &protocol, const TLS *tls, const Transport *transport)
    {
        if (transport == nullptr || transport->type.isEmpty()) return false;
        if (!singBoxTransportNeedsXray(transport->type)) return false;
        if (protocol == "vmess") return false;
        if (tls != nullptr && (tls->enabled || tls->reality->enabled)) return false;
        return true;
    }

    const char *coreUnsupportedTransportSkipReason()
    {
        return "Skipping unsupported config: transport needs Xray but profile has no TLS";
    }

    namespace {
        const QStringList &singBoxUtlsFingerprints()
        {
            static const QStringList fps = {"chrome", "firefox", "edge", "safari", "360", "qq", "ios", "android", "random", "randomized"};
            return fps;
        }

        bool isPlainTransportType(const QString &type)
        {
            return type.isEmpty() || type == "tcp" || type == "raw";
        }

        QString mapSingBoxUtlsFingerprint(const QString &fp)
        {
            if (fp.isEmpty() || fp == "unsafe") return "random";
            if (singBoxUtlsFingerprints().contains(fp)) return fp;
            if (fp.startsWith("chrome")) return "chrome";
            return "chrome";
        }

        void ensureSingBoxRealityUtls(QJsonObject &tls)
        {
            if (!tls["reality"].toObject()["enabled"].toBool()) return;
            auto utls = tls["utls"].toObject();
            const auto fp = utls["fingerprint"].toString();
            if (utls.isEmpty() || !utls["enabled"].toBool() || fp == "unsafe" || fp.isEmpty()
                || !singBoxUtlsFingerprints().contains(fp)) {
                tls["utls"] = QJsonObject{
                    {"enabled", true},
                    {"fingerprint", mapSingBoxUtlsFingerprint(fp)},
                };
            }
        }

        QString mapLegacyKcpHeaderType(const QString &type)
        {
            if (type.isEmpty() || type == "none") return {};
            if (type == "wechat-video") return "wechat";
            static const QStringList supported = {"dns", "dtls", "srtp", "utp", "wechat", "wireguard"};
            if (supported.contains(type)) return type;
            return {};
        }

        void applyXrayKcpStream(const QJsonObject &transport, QJsonObject &stream)
        {
            QJsonObject kcp = transport;
            kcp.remove("type");

            QString seed;
            if (kcp.contains("seed")) {
                seed = kcp["seed"].toString();
                kcp.remove("seed");
            }

            QString headerType;
            QString headerDomain;
            if (kcp.contains("header")) {
                const auto headerVal = kcp["header"];
                if (headerVal.isObject()) {
                    const auto header = headerVal.toObject();
                    headerType = header["type"].toString();
                    headerDomain = header["domain"].toString();
                } else if (headerVal.isString()) {
                    headerType = headerVal.toString();
                }
                kcp.remove("header");
            }

            if (!kcp.isEmpty()) stream["kcpSettings"] = kcp;

            const auto legacyHeader = mapLegacyKcpHeaderType(headerType);
            if (!legacyHeader.isEmpty() || !seed.isEmpty()) {
                QJsonObject settings;
                settings["header"] = legacyHeader;
                if (legacyHeader == "dns" && !headerDomain.isEmpty()) {
                    settings["value"] = headerDomain;
                } else if (!seed.isEmpty()) {
                    settings["value"] = seed;
                } else {
                    settings["value"] = "";
                }
                stream["finalmask"] = QJsonObject{
                    {"udp", QJsonArray{
                        QJsonObject{
                            {"type", "mkcp-legacy"},
                            {"settings", settings},
                        },
                    }},
                };
            }
        }

        QJsonObject singBoxStreamToXray(const QJsonObject &tls, const QJsonObject &transport)
        {
            QJsonObject stream;
            const auto network = transport["type"].toString();
            stream["network"] = network;

            if (tls["reality"].toObject()["enabled"].toBool()) {
                stream["security"] = "reality";
                const auto reality = tls["reality"].toObject();
                QJsonObject rs;
                rs["show"] = false;
                if (tls.contains("server_name")) rs["serverName"] = tls["server_name"];
                if (reality.contains("public_key")) rs["publicKey"] = reality["public_key"];
                if (reality.contains("short_id")) rs["shortId"] = reality["short_id"];
                rs["fingerprint"] = mapSingBoxUtlsFingerprint(tls["utls"].toObject()["fingerprint"].toString());
                stream["realitySettings"] = rs;
            } else if (tls["enabled"].toBool()) {
                stream["security"] = "tls";
                QJsonObject ts;
                if (tls.contains("server_name")) ts["serverName"] = tls["server_name"];
                if (tls["insecure"].toBool()) ts["allowInsecure"] = true;
                if (tls.contains("alpn")) ts["alpn"] = tls["alpn"];
                const auto fp = tls["utls"].toObject()["fingerprint"].toString();
                if (!fp.isEmpty() && fp != "unsafe") ts["fingerprint"] = fp;
                stream["tlsSettings"] = ts;
            } else {
                stream["security"] = "none";
            }

            if (network == "kcp") {
                applyXrayKcpStream(transport, stream);
            } else if (network == "ws") {
                QJsonObject ws;
                if (transport.contains("path")) ws["path"] = transport["path"];
                if (transport.contains("headers")) ws["headers"] = transport["headers"];
                if (transport.contains("max_early_data")) ws["maxEarlyData"] = transport["max_early_data"];
                if (transport.contains("early_data_header_name")) ws["earlyDataHeaderName"] = transport["early_data_header_name"];
                stream["wsSettings"] = ws;
            } else if (network == "grpc") {
                QJsonObject grpc;
                if (transport.contains("service_name")) grpc["serviceName"] = transport["service_name"];
                stream["grpcSettings"] = grpc;
            } else if (network == "http" || network == "httpupgrade") {
                QJsonObject http;
                if (transport.contains("host")) http["host"] = QListStr2QJsonArray({transport["host"].toString()});
                if (transport.contains("path")) http["path"] = transport["path"];
                if (transport.contains("method")) http["method"] = transport["method"];
                stream[network == "httpupgrade" ? "httpupgradeSettings" : "tcpSettings"] = http;
            }
            return stream;
        }
    }

    void normalizeSingBoxOutbound(QJsonObject &outbound)
    {
        if (outbound.contains("transport")) {
            if (isPlainTransportType(outbound["transport"].toObject()["type"].toString())) {
                outbound.remove("transport");
            }
        }
        if (outbound.contains("tls")) {
            auto tls = outbound["tls"].toObject();
            if (tls.contains("utls")) {
                auto utls = tls["utls"].toObject();
                const auto fp = utls["fingerprint"].toString();
                if (fp == "unsafe") {
                    if (tls["reality"].toObject()["enabled"].toBool()) {
                        utls["enabled"] = true;
                        utls["fingerprint"] = "random";
                        tls["utls"] = utls;
                    } else {
                        tls.remove("utls");
                    }
                } else if (!fp.isEmpty() && !singBoxUtlsFingerprints().contains(fp)) {
                    utls["fingerprint"] = fp.startsWith("chrome") ? QString("chrome") : QString("chrome");
                    tls["utls"] = utls;
                }
            }
            ensureSingBoxRealityUtls(tls);
            outbound["tls"] = tls;
        }
    }

    QJsonObject singBoxOutboundToXray(const QJsonObject &outbound)
    {
        const auto type = outbound["type"].toString();
        if (type.isEmpty()) return {};

        QJsonObject xray;
        xray["protocol"] = type;
        QJsonObject settings;
        if (type == "vless") {
            settings["address"] = outbound["server"];
            settings["port"] = outbound["server_port"];
            settings["id"] = outbound["uuid"];
            settings["encryption"] = "none";
            if (outbound.contains("flow")) settings["flow"] = outbound["flow"];
        } else if (type == "vmess") {
            settings["address"] = outbound["server"];
            settings["port"] = outbound["server_port"];
            settings["id"] = outbound["uuid"];
            settings["security"] = outbound.contains("security") ? outbound["security"] : QJsonValue("auto");
            if (outbound.contains("alter_id")) settings["alterId"] = outbound["alter_id"];
        } else if (type == "trojan") {
            settings["servers"] = QJsonArray{
                QJsonObject{
                    {"address", outbound["server"]},
                    {"port", outbound["server_port"]},
                    {"password", outbound["password"]},
                },
            };
        } else {
            return {};
        }
        xray["settings"] = settings;
        xray["streamSettings"] = singBoxStreamToXray(outbound["tls"].toObject(), outbound["transport"].toObject());
        return xray;
    }

    QJsonObject normalizeSingBoxConfig(const QJsonObject &config)
    {
        QJsonObject normalized = config;
        auto normalizeArray = [](const QJsonArray &arr) {
            QJsonArray result;
            for (const auto &v : arr) {
                auto obj = v.toObject();
                normalizeSingBoxOutbound(obj);
                result.append(obj);
            }
            return result;
        };
        if (normalized.contains("outbounds")) {
            normalized["outbounds"] = normalizeArray(normalized["outbounds"].toArray());
        }
        if (normalized.contains("endpoints")) {
            normalized["endpoints"] = normalizeArray(normalized["endpoints"].toArray());
        }
        return normalized;
    }

    QString getHeadersString(QStringList headers) {
        QString result;
        if (headers.length()%2 != 0) {
            return "";
        }
        QStringList formatted;
        formatted.reserve(headers.length()/2);

        for (int i=0;i<headers.length();i+=2) {
            formatted.append(QStringLiteral("%1=\"%2\"").arg(headers.at(i), headers.at(i + 1)));
        }
        return formatted.join(' ');
    }

    QStringList parseHeaderPairs(const QString& rawHeader) {
        bool inQuote = false;
        QString curr;
        QStringList list;
        for (const auto &ch: rawHeader) {
            if (inQuote) {
                if (ch == '"') {
                    inQuote = false;
                    list << curr;
                    curr = "";
                    continue;
                } else {
                    curr += ch;
                    continue;
                }
            }
            if (ch == '"') {
                inQuote = true;
                continue;
            }
            if (ch == ' ') {
                if (!curr.isEmpty()) {
                    list << curr;
                    curr = "";
                }
                continue;
            }
            if (ch == '=') {
                if (!curr.isEmpty()) {
                    list << curr;
                    curr = "";
                }
                continue;
            }
            curr+=ch;
        }
        if (!curr.isEmpty()) list<<curr;

        if (list.size()%2 != 0) {
            return {};
        }

        return list;
    }
}
