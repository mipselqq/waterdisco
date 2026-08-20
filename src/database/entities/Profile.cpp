#include <include/database/entities/Profile.h>

#include <QJsonDocument>
#include <QSet>

#include <algorithm>

#include "include/database/GroupsRepo.h"
#include "include/global/Configs.hpp"
#include "include/global/DiagLog.h"

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
        test_country.clear();
        ip_out.clear();
        latency = 0;
        latency_at = 0;
        ClearPerformanceTestResults();
    }

    void Profile::ClearPerformanceTestResults() {
        connect_time_ms = 0;
        rx_speed_mbps = 0.0;
        site_score = 0;
        performance_test_status = PerformanceTestStatus::Untested;
        performance_status_detail = {};
        dl_speed.clear();
        ul_speed.clear();
    }

    void Profile::SetPerformanceResult(int connectionTimeMs, double rxMbps,
                                       const QString &displaySpeed) {
        connect_time_ms = std::max(0, connectionTimeMs);
        rx_speed_mbps = std::max(0.0, rxMbps);
        site_score = CalculateSiteScore(connect_time_ms, rx_speed_mbps);
        performance_test_status = PerformanceTestStatus::Success;
        performance_status_detail = {};
        dl_speed = displaySpeed.isEmpty()
            ? QStringLiteral("%1 Mbps").arg(rx_speed_mbps, 0, 'f', 2)
            : displaySpeed;
        ul_speed.clear();
    }

    void Profile::MarkPerformanceSkipped(const PerformanceStatusDetail& detail) {
        rx_speed_mbps = 0.0;
        site_score = 0;
        performance_test_status = PerformanceTestStatus::Skipped;
        performance_status_detail = detail;
        dl_speed = QStringLiteral("Skipped");
        ul_speed = detail.reason;
        DIAG_LOG(QStringLiteral("MARK_SKIP id=%1 name=%2 type=%3 reasonEmpty=%4 reason=%5 measuredMs=%6 measuredMbps=%7 thresholdMs=%8 tooltip=%9")
                     .arg(id)
                     .arg(outbound ? outbound->name : name)
                     .arg(type)
                     .arg(detail.reason.isEmpty() ? "yes" : "no")
                     .arg(detail.reason)
                     .arg(detail.measuredMs)
                     .arg(detail.measuredMbps)
                     .arg(detail.thresholdMs)
                     .arg(DisplayRxSpeedTooltip().isEmpty() ? "EMPTY" : DisplayRxSpeedTooltip()));
    }

    void Profile::MarkPerformanceError(const PerformanceStatusDetail& detail) {
        if (connect_time_ms < 0) connect_time_ms = 0;
        rx_speed_mbps = 0.0;
        site_score = 0;
        performance_test_status = PerformanceTestStatus::Error;
        performance_status_detail = detail;
        dl_speed = QStringLiteral("Error");
        ul_speed = detail.reason;
        DIAG_LOG(QStringLiteral("MARK_ERROR id=%1 name=%2 type=%3 reasonEmpty=%4 reason=%5 measuredMs=%6 measuredMbps=%7 thresholdMs=%8 tooltip=%9")
                     .arg(id)
                     .arg(outbound ? outbound->name : name)
                     .arg(type)
                     .arg(detail.reason.isEmpty() ? "yes" : "no")
                     .arg(detail.reason)
                     .arg(detail.measuredMs)
                     .arg(detail.measuredMbps)
                     .arg(detail.thresholdMs)
                     .arg(DisplayRxSpeedTooltip().isEmpty() ? "EMPTY" : DisplayRxSpeedTooltip()));
    }

    void Profile::SetLatency(int ms) {
        latency = ms;
        // 0 means "never measured", so an explicit reset must clear the stamp
        // rather than record the moment we forgot the result.
        latency_at = ms == 0 ? 0 : QDateTime::currentSecsSinceEpoch();
    }

    QString Profile::DisplayTestResult() const {
        auto group = dataManager->groupsRepo->GetGroup(gid);
        if (group == nullptr) return "";
        QString result;
        if (!test_country.isEmpty()) result += UNICODE_LRO + CountryCodeToFlag(test_country) + " ";
        if (latency < 0) {
            result = "Unavailable";
            return result;
        } else if (latency > 0) {
            result += QString("%1 ms").arg(latency);
        }
        bool showSpeed = group->test_items_to_show == testShowItems::all || group->test_items_to_show == testShowItems::speedOnly;
        bool showIP = group->test_items_to_show == testShowItems::all || group->test_items_to_show == testShowItems::ipOnly;
        if (!dl_speed.isEmpty() && dl_speed != "N/A" && showSpeed) result += " ↓" + dl_speed;
        if (performance_test_status == PerformanceTestStatus::Success
            && !ul_speed.isEmpty() && ul_speed != "N/A" && showSpeed) {
            result += " ↑" + ul_speed;
        }
        if (!ip_out.isEmpty() && showIP) result += " 🌐" + ip_out;
        return result;
    }

    QString Profile::DisplayLatency() const {
        if (latency < 0) return QStringLiteral("Unavailable");
        if (latency == 0) return {};
        return QStringLiteral("%1 ms").arg(latency);
    }

    QString Profile::DisplayRxSpeed() const {
        switch (performance_test_status) {
        case PerformanceTestStatus::Success:
            if (!dl_speed.isEmpty()) return dl_speed;
            return QStringLiteral("%1 Mbps").arg(rx_speed_mbps, 0, 'f', 2);
        case PerformanceTestStatus::Skipped: return QStringLiteral("Skipped");
        case PerformanceTestStatus::Error: return QStringLiteral("Error");
        case PerformanceTestStatus::Untested: return {};
        }
        return {};
    }

    QString Profile::DisplayRxSpeedTooltip() const {
        const QString tip = FormatPerformanceStatusTooltip(performance_test_status, performance_status_detail);
        if (!tip.isEmpty()) return tip;
        if ((performance_test_status == PerformanceTestStatus::Skipped
             || performance_test_status == PerformanceTestStatus::Error)
            && !ul_speed.isEmpty() && ul_speed != QLatin1String("N/A")) {
            return ul_speed;
        }
        return {};
    }

    QString Profile::DisplayConnectionTime() const {
        if (connect_time_ms > 0) return QStringLiteral("%1 ms").arg(connect_time_ms);
        if (performance_test_status == PerformanceTestStatus::Error) {
            return QStringLiteral("Unavailable");
        }
        return {};
    }

    QString Profile::DisplaySiteScore() const {
        if (performance_test_status != PerformanceTestStatus::Success) return {};
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

    QString Profile::DisplayTraffic() const {
        if (traffic_downlink + traffic_uplink == 0) return "";
        return UNICODE_LRO + QString("%1↑ %2↓").arg(ReadableSize(traffic_uplink), ReadableSize(traffic_downlink));
    }

    QString Profile::DisplayTrafficRx() const {
        if (traffic_downlink == 0) return {};
        return UNICODE_LRO + ReadableSize(traffic_downlink);
    }

    QString Profile::DisplayTrafficTx() const {
        if (traffic_uplink == 0) return {};
        return UNICODE_LRO + ReadableSize(traffic_uplink);
    }

    void Profile::ResetTraffic() {
        traffic_downlink = 0;
        traffic_uplink = 0;
    }

        QString ProfileFilter_ent_key(const std::shared_ptr<Configs::Profile> &ent, bool ignoreMetadata) {
        auto key = ent->outbound->ExportJsonLink(ignoreMetadata);
        return key;
    }

    QString ProfileFilter_ent_identity_key(const std::shared_ptr<Configs::Profile> &ent) {
        auto obj = ent->outbound->ExportIdentity();
        return ent->type + "|" + QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
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
        QMap<QString, QList<std::shared_ptr<Profile>>> srcByKey;

        for (const auto &ent: src) {
            srcByKey[ProfileFilter_ent_key(ent, ignoreMetadata)].append(ent);
        }
        // A src entry can be claimed once. Handing the same one to every dst
        // duplicate makes the caller map them all onto a single id, which
        // collapses N identical servers into N slots of one profile (#1775).
        for (const auto &ent: dst) {
            auto it = srcByKey.find(ProfileFilter_ent_key(ent, ignoreMetadata));
            if (it == srcByKey.end() || it->isEmpty()) continue;
            outDst += ent;
            outSrc += it->takeFirst();
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

    void ProfileFilter::ChangedByIdentity(QList<std::shared_ptr<Profile>> &src,
                                          QList<std::shared_ptr<Profile>> &dst,
                                          QList<std::shared_ptr<Profile>> &changedSrc,
                                          QList<std::shared_ptr<Profile>> &changedDst) {
        QMap<QString, QList<std::shared_ptr<Profile>>> srcByKey;
        for (const auto &ent: src) {
            srcByKey[ProfileFilter_ent_identity_key(ent)].append(ent);
        }

        QSet<Profile *> matchedSrc;
        QList<std::shared_ptr<Profile>> remainingDst;
        for (const auto &ent: dst) {
            auto &bucket = srcByKey[ProfileFilter_ent_identity_key(ent)];
            if (bucket.isEmpty()) {
                remainingDst += ent;
                continue;
            }
            auto srcEnt = bucket.takeFirst();
            changedSrc += srcEnt;
            changedDst += ent;
            matchedSrc.insert(srcEnt.get());
        }

        QList<std::shared_ptr<Profile>> remainingSrc;
        for (const auto &ent: src) {
            if (!matchedSrc.contains(ent.get())) remainingSrc += ent;
        }
        src = remainingSrc;
        dst = remainingDst;
    }
}
