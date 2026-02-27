#pragma once

#include <QString>
#include <QList>
#include <QMutex>

#include "include/database/entities/Profile.h"

namespace Stats {
    struct TrafficLooperEntry {
        QString tag;
        Configs::Profile* ent = nullptr;  // nullptr for "direct"
        long long downlink = 0;
        long long uplink = 0;
        long long last_update = 0;
        double downlink_rate = 0;
        double uplink_rate = 0;
        bool is_chain_tail = false;
        bool ignore_for_rate = false;
    };

    inline QString DisplaySpeed(const std::shared_ptr<TrafficLooperEntry> &entry) {
        return UNICODE_LRO + QString("%1↑ %2↓").arg(ReadableSize(entry->uplink_rate), ReadableSize(entry->downlink_rate));
    }

    class TrafficLooper {
    public:
        bool loop_enabled = false;
        bool looping = false;
        QMutex loop_mutex;

        bool isChain;
        std::shared_ptr<TrafficLooperEntry> proxy;
        std::shared_ptr<TrafficLooperEntry> direct;

        void UpdateAll();

        void Loop();

        // (profile, tag) pairs from config build
        void SetEnts(const QList<std::pair<std::shared_ptr<Configs::Profile>, QString>>& profsAndTags);

    private:
        QList<std::shared_ptr<Configs::Profile>> profiles;
        QList<TrafficLooperEntry> entries;
    };

    extern TrafficLooper *trafficLooper;
} // namespace Stats
