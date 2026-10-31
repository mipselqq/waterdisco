#pragma once
#include "include/configs/common/multiplex.h"
#include "include/configs/common/Outbound.h"
#include "include/configs/common/TLS.h"
#include "include/configs/common/transport.h"
#include "include/configs/common/utils.h"

namespace Configs
{
    inline QStringList vlessFlows = {"xtls-rprx-vision"};

    namespace Checks {
        // sing-box takes only xtls-rprx-vision, and Vision works only on TLS (or REALITY) directly over TCP.
        QString VlessFlow(const QString &flow, bool tlsEnabled, const QString &transportType);
    }

    class vless : public outbound
    {
        public:
        QString uuid;
        QString flow;
        std::shared_ptr<TLS> tls = std::make_shared<TLS>();
        QString packet_encoding = "xudp";
        std::shared_ptr<Multiplex> multiplex = std::make_shared<Multiplex>();
        std::shared_ptr<Transport> transport = std::make_shared<Transport>();

        bool HasTLS() override {
            return true;
        }

        bool HasMux() override {
            return true;
        }

        bool HasTransport() override {
            return true;
        }

        std::shared_ptr<TLS> GetTLS() override {
            return tls;
        }

        std::shared_ptr<Multiplex> GetMux() override {
            return multiplex;
        }

        std::shared_ptr<Transport> GetTransport() override {
            return transport;
        }

        bool ParseFromLink(const QString& link) override;
        bool ParseFromJson(const QJsonObject& object) override;
        bool ParseFromClash(const clash::Proxies& object) override;
        QString ExportToLink() override;
        QJsonObject ExportToJson() override;
        BuildResult Build() override;
        BuildResult BuildXray() override;

        bool IsXray() override {
            const auto network = transport->type.isEmpty() ? QStringLiteral("raw") : NormalizeXrayNetwork(transport->type);
            const bool realityEnabled = tls && tls->reality && tls->reality->enabled;
            const bool xraySupportsProfile = XrayNetworks.contains(network) &&
                (!realityEnabled || Checks::XrayRealityNetwork(QStringLiteral("reality"), network).isEmpty());
            if (Configs::dataManager->settingsRepo->preferred_core == Xray::XrayCore && xraySupportsProfile) return true;
            // sing-box supports Vision, but not its udp443 variant.
            if (flow.compare(QLatin1String("xtls-rprx-vision-udp443"), Qt::CaseInsensitive) == 0) return true;
            // mKCP is an Xray transport and is not available in sing-box.
            if (transport->type.compare(QLatin1String("kcp"), Qt::CaseInsensitive) == 0) return !realityEnabled;
            if (!singBoxTransportNeedsXray(transport->type)) return false;
            return tls && (tls->enabled || realityEnabled);
        }

        QString DisplayType() override;

        void Validate(Validation::Issues &issues) override;
    };
}
