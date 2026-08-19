#include "include/global/HTTPRequestHelper.hpp"

#include <QNetworkProxy>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QFile>
#include <QApplication>
#include "include/global/Configs.hpp"
#include "include/ui/mainwindow.h"

namespace Configs_network {

    namespace {
        constexpr qsizetype kMaxHeaderValueLength = 1000;

        bool isSafeHeaderValue(const QString& value) {
            return !value.isEmpty() && value.size() <= kMaxHeaderValueLength &&
                   !value.contains('\r') && !value.contains('\n');
        }

        void setSafeHeader(QNetworkRequest& request, const QByteArray& name, const QString& value) {
            if (isSafeHeaderValue(value)) request.setRawHeader(name, value.toUtf8());
        }
    }

    void NetworkRequestHelper::ConfigureAccessManager(QNetworkAccessManager& manager) {
        const auto* settings = Configs::dataManager->settingsRepo.get();
        // A disabled profile or an absent connection must never turn an update
        // or a new subscription into a hard failure: route it directly instead.
        if (!settings->net_use_proxy || settings->started_id < 0) return;

        QNetworkProxy proxy;
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(settings->inbound_address == "::" ? "127.0.0.1" : settings->inbound_address);
        proxy.setPort(settings->inbound_socks_port);
        if (settings->inbound_auth) {
            proxy.setUser(settings->inbound_user);
            proxy.setPassword(settings->inbound_pass);
        }
        manager.setProxy(proxy);
    }

    void NetworkRequestHelper::ConfigureRequest(QNetworkRequest& request, bool sendIdentity) {
        const auto* settings = Configs::dataManager->settingsRepo.get();
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        const QString userAgent = settings->GetUserAgent();
        if (isSafeHeaderValue(userAgent)) {
            request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, userAgent);
        }
        if (settings->net_insecure) {
            QSslConfiguration ssl;
            ssl.setPeerVerifyMode(QSslSocket::PeerVerifyMode::VerifyNone);
            request.setSslConfiguration(ssl);
        }
        if (!sendIdentity || !settings->sub_send_hwid) return;
        setSafeHeader(request, "X-Device-Model", settings->sub_device_model);
        setSafeHeader(request, "X-Device-OS", settings->sub_device_os);
        setSafeHeader(request, "X-HWID", settings->sub_hwid);
        setSafeHeader(request, "X-Ver-OS", settings->sub_ver_os);
    }

    HTTPResponse NetworkRequestHelper::HttpGet(const QString &url, bool sendHwid, bool useProxy) {
        QNetworkRequest request;
        QNetworkAccessManager accessManager;
        accessManager.setTransferTimeout(10000);
        request.setUrl(url);
        Q_UNUSED(useProxy);
        ConfigureAccessManager(accessManager);
        ConfigureRequest(request, sendHwid);
        //
        auto _reply = accessManager.get(request);
        connect(_reply, &QNetworkReply::sslErrors, _reply, [](const QList<QSslError> &errors) {
            QStringList error_str;
            for (const auto &err: errors) {
                error_str << err.errorString();
            }
            MW_show_log(QString("SSL Errors: %1 %2").arg(error_str.join(","), Configs::dataManager->settingsRepo->net_insecure ? "(Ignored)" : ""));
        });
        // Wait for response
        QEventLoop loop;
        connect(_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        //
        auto result = HTTPResponse{_reply->error() == QNetworkReply::NetworkError::NoError ? "" : _reply->errorString(),
                                       _reply->readAll(), _reply->rawHeaderPairs()};
        _reply->deleteLater();
        return result;
    }

    HTTPResponse NetworkRequestHelper::HttpGetDirect(const QString &url) {
        QNetworkRequest request{QUrl(url)};
        QNetworkAccessManager accessManager;
        accessManager.setTransferTimeout(10000);
        accessManager.setProxy(QNetworkProxy::NoProxy);
        ConfigureRequest(request);
        auto *reply = accessManager.get(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        HTTPResponse result{reply->error() == QNetworkReply::NoError ? "" : reply->errorString(),
                            reply->readAll(), reply->rawHeaderPairs()};
        reply->deleteLater();
        return result;
    }

    QString NetworkRequestHelper::GetHeader(const QList<QPair<QByteArray, QByteArray>> &header, const QString &name) {
        for (const auto &p: header) {
            if (QString(p.first).toLower() == name.toLower()) return p.second;
        }
        return "";
    }

    QString NetworkRequestHelper::DownloadAsset(const QString &url, const QString &fileName) {
        QNetworkRequest request;
        QNetworkAccessManager accessManager;
        request.setUrl(url);
        ConfigureAccessManager(accessManager);
        ConfigureRequest(request);

        auto _reply = accessManager.get(request);
        connect(_reply, &QNetworkReply::sslErrors, _reply, [](const QList<QSslError> &errors) {
            QStringList error_str;
            for (const auto &err: errors) {
                error_str << err.errorString();
            }
            MW_show_log(QString("SSL Errors: %1 %2").arg(error_str.join(","), Configs::dataManager->settingsRepo->net_insecure ? "(Ignored)" : ""));
        });
        connect(_reply, &QNetworkReply::downloadProgress, _reply, [&](qint64 bytesReceived, qint64 bytesTotal)
        {
            runOnUiThread([=]{
                GetMainWindow()->setDownloadReport(DownloadProgressReport{fileName, bytesReceived, bytesTotal}, true);
                GetMainWindow()->UpdateDataView();
            });
        });
        QEventLoop loop;
        connect(_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        runOnUiThread([=]
        {
            GetMainWindow()->setDownloadReport({}, false);
            GetMainWindow()->UpdateDataView(true);
        });
        auto netErr = _reply->error();
        const QString netErrStr = _reply->errorString();
        const int httpStatus = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = _reply->readAll();
        _reply->deleteLater();

        if (netErr != QNetworkReply::NetworkError::NoError) {
            return netErrStr;
        }

        if (httpStatus != 0 && (httpStatus < 200 || httpStatus >= 300)) {
            return QObject::tr("Download failed: server returned HTTP status %1.").arg(httpStatus);
        }
        if (body.isEmpty()) {
            return QObject::tr("Download failed: the server returned an empty response.");
        }

        const auto filePath = Configs::GetBasePath() + "/" + fileName;
        const auto tmpPath = filePath + ".tmp";
        QFile tmp(tmpPath);
        if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return QObject::tr("Could not open file.");
        }
        if (tmp.write(body) != body.size() || !tmp.flush()) {
            tmp.close();
            tmp.remove();
            return QObject::tr("Could not write file.");
        }
        tmp.close();
        QFile::remove(filePath);
        if (!tmp.rename(filePath)) {
            tmp.remove();
            return QObject::tr("Could not save downloaded file.");
        }
        return "";
    }

} // namespace Configs_network
