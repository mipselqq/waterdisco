#pragma once
#include "DialFields.h"
#include "multiplex.h"
#include "TLS.h"
#include "transport.h"
#include "include/configs/baseConfig.h"

namespace Configs
{
    class outbound : public baseConfig
    {
    public:
        QString name;
        QString server;
        int server_port = 0;
        std::shared_ptr<DialFields> dialFields = std::make_shared<DialFields>();

        outbound()
        {
            _add(new configItem("name", &name, string));
            _add(new configItem("server", &server, string));
            _add(new configItem("server_port", &server_port, integer));
            _add(new configItem("dial_fields", dynamic_cast<JsonStore *>(dialFields.get()), jsonStore));
        }

        void ResolveDomainToIP(const std::function<void()> &onFinished);

        virtual QString GetAddress()
        {
            return server;
        }

        virtual QString DisplayAddress()
        {
            return ::DisplayAddress(server, server_port);
        }

        virtual QString DisplayName()
        {
            if (name.isEmpty()) {
                return DisplayAddress();
            }
            return name;
        }

        virtual QString DisplayType() { return {}; };

        QString DisplayTypeAndName()
        {
            return QString("[%1] %2").arg(DisplayType(), DisplayName());
        }

        virtual bool HasMux() { return false; }

        virtual bool HasTransport() { return false; }

        virtual bool HasTLS() { return false; }

        virtual std::shared_ptr<TLS> GetTLS() { return std::make_shared<TLS>(); }

        virtual std::shared_ptr<Transport> GetTransport() { return std::make_shared<Transport>(); }

        virtual std::shared_ptr<Multiplex> GetMux() { return std::make_shared<Multiplex>(); }

        virtual bool IsEndpoint() { return false; };

        // baseConfig overrides
        bool ParseFromLink(const QString& link) override;
        bool ParseFromJson(const QJsonObject& object) override;
        QString ExportToLink() override;
        QJsonObject ExportToJson() override;
        BuildResult Build() override;
    };
}
