#pragma once

#include <QList>
#include <QMap>
#include <QMutex>
#include <QTableWidget>

#include "include/stats/connections/connectionLister.hpp"

class ConnectionsPresenter final {
public:
    explicit ConnectionsPresenter(QTableWidget* table);

    void setup();

    void update(const QMap<QString, Stats::ConnectionMetadata>& to_update,
                const QMap<QString, Stats::ConnectionMetadata>& to_add);

    void recreate(const QList<Stats::ConnectionMetadata>& connections);

private:
    QTableWidget* table = nullptr;
    QMutex connection_list_mutex;
    int tooltip_id = 0;
};
