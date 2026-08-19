#pragma once

#include <QObject>
#include <functional>

class QNetworkAccessManager;
class QNetworkRequest;

namespace Configs_network {
    struct HTTPResponse {
        QString error;
        QByteArray data;
        QList<QPair<QByteArray, QByteArray>> header;
    };

    struct DownloadProgressReport
    {
        QString fileName;
        qint64 downloadedSize;
        qint64 totalSize;
    };

    class NetworkRequestHelper : QObject {
        Q_OBJECT

        explicit NetworkRequestHelper(QObject *parent) : QObject(parent){};

        ~NetworkRequestHelper() override = default;
        ;

    public:
        // Applies the one persisted routing policy to every app HTTP client.
        // It deliberately falls back to direct networking if no profile is up.
        static void ConfigureAccessManager(QNetworkAccessManager &manager);

        static void ConfigureRequest(QNetworkRequest &request, bool sendIdentity = false);

        static HTTPResponse HttpGet(const QString &url, bool sendHwid = false, bool useProxy = false);

        static QString GetHeader(const QList<QPair<QByteArray, QByteArray>> &header, const QString &name);

        static QString DownloadAsset(const QString &url, const QString &fileName);
    };
} // namespace Configs_network

using namespace Configs_network;
