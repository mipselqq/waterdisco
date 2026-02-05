#pragma once

#include <QString>
#include <QList>
#include <QMutex>

#include "TrafficData.hpp"
#include "include/database/entities/Profile.h"

namespace Stats {
    class TrafficLooper {
    public:
        bool loop_enabled = false;
        bool looping = false;
        QMutex loop_mutex;

        bool isChain;
        std::shared_ptr<TrafficData> proxy;
        std::shared_ptr<TrafficData> direct;

        void UpdateAll();

        void Loop();

        void SetEnts(const QList<std::shared_ptr<Configs::Profile>>& profs);

    private:
        QList<std::shared_ptr<Configs::Profile>> profiles;
        QList<std::shared_ptr<TrafficData>> items;
    };

    extern TrafficLooper *trafficLooper;
} // namespace Stats
