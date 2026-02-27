#pragma once
#include <QString>

namespace Configs_network {
    const QString warpApiURL = "https://api.cloudflareclient.com/v0a737/reg";

    struct warpConfig {
        QString privateKey;
        QString publicKey;
        QString endpoint;
    };

    std::shared_ptr<warpConfig> genWarpConfig(QString *error, QString privateKey, QString publicKey);
}
