#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <include/configs/sub/warp.h>
#include <include/global/Configs.hpp>
#include <include/global/HTTPRequestHelper.hpp>
#include <QObject>
#include <utility>


namespace Configs_network {
    std::shared_ptr<warpConfig> genWarpConfig(QString *error, QString privateKey, QString publicKey) {
        std::shared_ptr<warpConfig> config = std::make_shared<warpConfig>();

        auto OSStr = getOSString();
        QJsonObject payload = {
            {"key", publicKey},
            {"install_id", ""},
            {"warp_enabled", true},
            {"tos", QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.000+00:00")},
            {"type", OSStr},
            {"locale", "en_US"},
        };

        QNetworkRequest request;
        QNetworkAccessManager accessManager;
        accessManager.setTransferTimeout(10000);
        request.setUrl(warpApiURL);
        NetworkRequestHelper::ConfigureAccessManager(accessManager);
        NetworkRequestHelper::ConfigureRequest(request);
        request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, "WARP for Android");
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        auto _reply = accessManager.post(request, QJsonObject2QString(payload, true).toUtf8());
        QEventLoop loop;
        QObject::connect(_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (_reply->error() != QNetworkReply::NoError) {
            *error = _reply->errorString();
            return config;
        }

        auto rawResponse = QString::fromUtf8(_reply->readAll());
        auto jsonResp = QString2QJsonObject(rawResponse)["config"].toObject();
        if (!jsonResp.contains("peers") || !jsonResp["peers"].isArray() || jsonResp["peers"].toArray().empty()) {
            *error = "Received invalid response: " + rawResponse;
            return config;
        }

        auto reservedByteArray = DecodeB64IfValid(jsonResp["client_id"].toString());
        for (char byte : reservedByteArray) {
            config->reserved << static_cast<unsigned char>(byte);
        }

        auto peerObj = jsonResp["peers"].toArray()[0].toObject();
        if (peerObj.isEmpty()) {
            *error = "Received invalid response: " + rawResponse;
            return config;
        }

        config->privateKey = std::move(privateKey);
        if (auto pubKey = peerObj["public_key"].toString(); !pubKey.isEmpty()) {
            config->publicKey = pubKey;
        } else {
            *error = "Received invalid response: " + rawResponse;
            return config;
        }

        config->endpoint = peerObj["endpoint"].toObject()["host"].toString();
        if (config->endpoint.isEmpty()) {
            config->endpoint = "engage.cloudflareclient.com:2408";
        }

        auto ifcAddrObj = jsonResp["interface"].toObject()["addresses"].toObject();
        if (ifcAddrObj.isEmpty()) {
            *error = "Received invalid response: " + rawResponse;
            return config;
        }
        config->ipv4Address = ifcAddrObj["v4"].toString();
        config->ipv6Address = ifcAddrObj["v6"].toString();

        return config;
    }
}
