#pragma once

#include "include/configs/common/Outbound.h"
#include "include/configs/common/TLS.h"

namespace Configs
{
    class http : public outbound
    {
        public:
        QString username;
        QString password;
        QString path;
        QStringList headers;
        std::shared_ptr<TLS> tls = std::make_shared<TLS>();

        http() : outbound()
        {
            _add(new configItem("username", &username, string));
            _add(new configItem("password", &password, string));
            _add(new configItem("path", &path, string));
            _add(new configItem("headers", &headers, stringList));
            _add(new configItem("tls", dynamic_cast<JsonStore *>(tls.get()), jsonStore));
        }

        bool HasTLS() override {
            return true;
        }

        std::shared_ptr<TLS> GetTLS() override {
            return tls;
        }

        // baseConfig overrides
        bool ParseFromLink(const QString& link) override;
        bool ParseFromJson(const QJsonObject& object) override;
        QString ExportToLink() override;
        QJsonObject ExportToJson() override;
        BuildResult Build() override;

        QString DisplayType() override;
    };
}
