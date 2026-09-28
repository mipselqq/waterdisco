#pragma once

#include <QObject>
#include <QSsl>
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

    struct HttpGetOptions {
        bool useProxy = false;
        // > 0 aborts the transfer once the body exceeds it and reports an error.
        qint64 maxBytes = 0;
        // Empty sends the global User-Agent.
        QString userAgent;
        QList<QPair<QByteArray, QByteArray>> headers;
        QSsl::SslProtocol tlsProtocol = QSsl::SecureProtocols;
        bool http2 = true;
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

        static HTTPResponse HttpGet(const QString &url, const HttpGetOptions &options);

        // Diagnostic requests that must never inherit the application's proxy policy.
        static HTTPResponse HttpGetDirect(const QString &url);

        static QString GetHeader(const QList<QPair<QByteArray, QByteArray>> &header, const QString &name);

        static QString DownloadAsset(const QString &url, const QString &fileName, bool useProxy = false);
    };
} // namespace Configs_network

using namespace Configs_network;
