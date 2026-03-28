#include <include/database/entities/Profile.h>

#include "include/database/GroupsRepo.h"
#include "include/global/Configs.hpp"

namespace Configs
{
    Profile::Profile(Configs::outbound *outbound, const QString &type_)
    {
        if (!type_.isEmpty()) this->type = type_;


        if (outbound != nullptr) {
            this->outbound = std::shared_ptr<Configs::outbound>(outbound);
        }
    }

    void Profile::ClearTestResults() {
        ip_out.clear();
        latency = 0;
        connect_time_ms = 0;
        site_score = 0;
        dl_speed.clear();
        ul_speed.clear();
        dl_speed_mbps = 0.0;
        ul_speed_mbps = 0.0;
    }

    QString Profile::DisplayLatency() const {
        if (latency < 0) return "Unavailable";
        if (latency == 0) return "";
        return QString("%1 ms").arg(latency);
    }

    QString Profile::DisplayTxSpeed() const {
        return ul_speed;
    }

    QString Profile::DisplayRxSpeed() const {
        return dl_speed;
    }

    QString Profile::DisplayConnectionTime() const {
        if (connect_time_ms < 0) return "Unavailable";
        if (connect_time_ms == 0) return "";
        return QString("%1 ms").arg(connect_time_ms);
    }

    QString Profile::DisplaySiteScore() const {
        if (site_score <= 0) return "";
        return QString::number(site_score);
    }

    QColor Profile::DisplayLatencyColor() const {
        if (latency < 0) {
            return Qt::darkGray;
        } else if (latency > 0) {
            if (latency <= 100) {
                return Qt::darkGreen;
            } else if (latency <= 300)
            {
                return Qt::darkYellow;
            } else {
                return Qt::red;
            }
        } else {
            return {};
        }
    }

    QString Profile::DisplayTrafficTx() const {
        if (traffic_uplink == 0) return "";
        return UNICODE_LRO + ReadableSize(traffic_uplink);
    }

    QString Profile::DisplayTrafficRx() const {
        if (traffic_downlink == 0) return "";
        return UNICODE_LRO + ReadableSize(traffic_downlink);
    }

    void Profile::ResetTraffic() {
        traffic_downlink = 0;
        traffic_uplink = 0;
    }

        QString ProfileFilter_ent_key(const std::shared_ptr<Configs::Profile> &ent, bool ignoreMetadata) {
        auto key = ent->outbound->ExportJsonLink(ignoreMetadata);
        return key;
    }

    void ProfileFilter::Uniq(const QList<std::shared_ptr<Profile>> &in,
                             QList<std::shared_ptr<Profile>> &out,
                             bool keep_last, bool ignoreMetadata) {
        QMap<QString, std::shared_ptr<Profile>> hashMap;

        for (const auto &ent: in) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            if (hashMap.contains(key)) {
                if (keep_last) {
                    out.removeAll(hashMap[key]);
                    hashMap[key] = ent;
                    out += ent;
                }
            } else {
                hashMap[key] = ent;
                out += ent;
            }
        }
    }

    void ProfileFilter::Common(const QList<std::shared_ptr<Profile>> &src,
                               const QList<std::shared_ptr<Profile>> &dst,
                               QList<std::shared_ptr<Profile>> &outSrc,
                               QList<std::shared_ptr<Profile>> &outDst,
                               bool ignoreMetadata) {
        QMap<QString, std::shared_ptr<Profile>> hashMap;

        for (const auto &ent: src) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            hashMap[key] = ent;
        }
        for (const auto &ent: dst) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            if (hashMap.contains(key)) {
                outDst += ent;
                outSrc += hashMap[key];
            }
        }
    }

    void ProfileFilter::OnlyInSrc(const QList<std::shared_ptr<Profile>> &src,
                                  const QList<std::shared_ptr<Profile>> &dst,
                                  QList<std::shared_ptr<Profile>> &out,
                                  bool ignoreMetadata) {
        QMap<QString, bool> hashMap;

        for (const auto &ent: dst) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            hashMap[key] = true;
        }
        for (const auto &ent: src) {
            QString key = ProfileFilter_ent_key(ent, ignoreMetadata);
            if (!hashMap.contains(key)) out += ent;
        }
    }

    void ProfileFilter::OnlyInSrc_ByPointer(const QList<std::shared_ptr<Profile>> &src,
                                            const QList<std::shared_ptr<Profile>> &dst,
                                            QList<std::shared_ptr<Profile>> &out) {
        for (const auto &ent: src) {
            if (!dst.contains(ent)) out += ent;
        }
    }
}
