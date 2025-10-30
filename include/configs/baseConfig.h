#pragma once

#include <QString>
#include <include/global/Utils.hpp>

#include "generate.h"
#include "common/Outbound.h"
#include "include/global/ConfigItem.hpp"

namespace Configs
{
    class baseConfig : public JsonStore
    {
    public:
        virtual bool ParseFromLink(const QString& link);

        virtual bool ParseFromJson(const QJsonObject& object);

        virtual QString ExportToLink();

        virtual QJsonObject ExportToJson();

        virtual BuildResult Build();
    };

    class outbound : public baseConfig
    {
        public:
        std::shared_ptr<OutboundCommons> commons = std::make_shared<OutboundCommons>();

        void ResolveDomainToIP(const std::function<void()> &onFinished);

        virtual QString DisplayAddress()
        {
            return ::DisplayAddress(commons->server, commons->server_port);
        }

        QString DisplayName()
        {
            if (commons->name.isEmpty()) {
                return DisplayAddress();
            }
            return commons->name;
        }

        virtual QString DisplayType() { return {}; };

        QString DisplayTypeAndName()
        {
            return QString("[%1] %2").arg(DisplayType(), DisplayName());
        }

        static bool IsEndpoint() { return false; };
    };
}
