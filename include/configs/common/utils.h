#pragma once
#include <QJsonObject>
#include <QUrlQuery>

namespace Configs
{
    class TLS;
    class Transport;

    void mergeUrlQuery(QUrlQuery& baseQuery, const QString& strQuery);

    void mergeJsonObjects(QJsonObject& baseObject, const QJsonObject& obj);

    QStringList jsonObjectToQStringList(const QJsonObject& obj);

    QJsonObject qStringListToJsonObject(const QStringList& list);

    bool useXrayVless(const QString& link);

    // Transports sing-box cannot dial natively (kcp, ...) are run through Xray.
    bool singBoxTransportNeedsXray(const QString &type);

    // VLESS/Trojan over KCP (etc.) without TLS cannot run on sing-box or current Xray
    // for public servers; VMess is fine (protocol encryption).
    bool isCoreUnsupportedTransport(const QString &protocol, const TLS *tls, const Transport *transport);
    const char *coreUnsupportedTransportSkipReason();

    // Xray uses network "raw" and uTLS fingerprint "unsafe"; sing-box expects
    // no transport block and maps unsafe away from Reality (which requires uTLS).
    void normalizeSingBoxOutbound(QJsonObject &outbound);
    QJsonObject normalizeSingBoxConfig(const QJsonObject &config);

    // Convert a sing-box outbound object to an Xray outbound object.
    QJsonObject singBoxOutboundToXray(const QJsonObject &outbound);

    QString getHeadersString(QStringList headers);

    QStringList parseHeaderPairs(const QString& rawHeader);
}
