#pragma once
#include "include/configs/baseConfig.h"

namespace Configs
{
    class Transport : public baseConfig
    {
        public:
        QString type;

        // HTTP
        QString host;
        QString path;
        QString method;
        QStringList headers;
        QString idle_timeout;
        QString ping_timeout;

        // Websocket
        int max_early_data = 0;
        QString early_data_header_name;

        // gRPC
        QString service_name;

        // Unparsed transport keys (kcp seed/mtu/header, etc.) kept for Xray conversion.
        QJsonObject extra;

        QJsonObject ExportFullJson();

        // baseConfig overrides
        bool ParseFromLink(const QString& link) override;
        bool ParseFromJson(const QJsonObject& object) override;
        bool ParseFromClash(const clash::Proxies& object) override;
        QString ExportToLink() override;
        QJsonObject ExportToJson() override;
        QJsonObject ExportIdentity() override;
        BuildResult Build() override;
    };
}
