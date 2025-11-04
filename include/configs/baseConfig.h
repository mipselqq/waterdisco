#pragma once

#include <QJsonObject>
#include "include/global/Configs.hpp"

namespace Configs
{
    struct BuildResult {
        QJsonObject object;
        QString error;
    };

    class baseConfig : public JsonStore
    {
    public:
        virtual bool ParseFromLink(const QString& link);

        virtual bool ParseFromJson(const QJsonObject& object);

        virtual QString ExportToLink();

        virtual QJsonObject ExportToJson();

        virtual BuildResult Build();
    };
}
