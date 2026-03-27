#pragma once

#include "Const.hpp"
#include "Utils.hpp"
#include "include/database/DatabaseManager.h"
#include <map>
#include <string>

// Built-in routing rule-set URL mapping.
// Keep this in a header as an inline object, since several UI/config units use it.
inline const std::map<std::string, std::string> ruleSetMap = {
    {"geoip-cn", "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/geoip-cn.srs"},
    {"geoip-private", "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/geoip-private.srs"},
    {"geoip-ir", "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/geoip-ir.srs"},
    {"geosite-cn", "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-cn.srs"},
    {"geosite-geolocation-!cn", "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-geolocation-!cn.srs"},
    {"geosite-category-ads-all", "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-category-ads-all.srs"},
};

// Switch core support

namespace Configs {
    void initDB(const std::string& dbPath);

    QString FindCoreRealPath();

    bool IsAdmin(bool forceRenew=false);

    bool isSetuidSet(const std::string& path);

    QString GetBasePath();

    bool HasNaive();
} // namespace Configs

#define ROUTES_PREFIX_NAME QString("route_profiles")
#define ROUTES_PREFIX QString(ROUTES_PREFIX_NAME + "/")
