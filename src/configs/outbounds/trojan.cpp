#include "include/configs/outbounds/trojan.h"

#include <QUrlQuery>
#include <include/global/Utils.hpp>

#include "include/configs/common/utils.h"

namespace Configs {
    bool Trojan::ParseFromLink(const QString& link)
    {
        auto url = QUrl(link);
        if (!url.isValid()) return false;

        outbound::ParseFromLink(link);
        password = url.userName();
        tls->ParseFromLink(link);
        transport->ParseFromLink(link);
        multiplex->ParseFromLink(link);
        return true;
    }
    bool Trojan::ParseFromJson(const QJsonObject& object)
    {
        if (object.isEmpty() || object["type"].toString() != "trojan") return false;
        outbound::ParseFromJson(object);
        if (object.contains("password")) password = object["password"].toString();
        if (object.contains("tls")) tls->ParseFromJson(object["tls"].toObject());
        if (object.contains("transport")) transport->ParseFromJson(object["transport"].toObject());
        if (object.contains("multiplex")) multiplex->ParseFromJson(object["multiplex"].toObject());
        return true;
    }
    bool Trojan::ParseFromClash(const clash::Proxies& object)
    {
        if (object.type != "trojan") return false;
        outbound::ParseFromClash(object);
        password = QString::fromStdString(object.password);

        tls->ParseFromClash(object);
        tls->enabled = true;
        transport->ParseFromClash(object);
        multiplex->ParseFromClash(object);
        return true;
    }
    QString Trojan::ExportToLink()
    {
        QUrl url;
        QUrlQuery query;
        url.setHost(server);
        url.setPort(server_port);
        url.setScheme("trojan");
        if (!name.isEmpty()) url.setFragment(name);
        url.setUserName(password);
        if (tls->enabled) mergeUrlQuery(query, tls->ExportToLink());
        if (!transport->type.isEmpty()) mergeUrlQuery(query, transport->ExportToLink());
        if (multiplex->enabled) mergeUrlQuery(query, multiplex->ExportToLink());
        if (!query.isEmpty()) url.setQuery(query);
        return url.toString(QUrl::FullyEncoded);
    }
    QJsonObject Trojan::ExportToJson()
    {
        QJsonObject object;
        object["type"] = "trojan";
        mergeJsonObjects(object, outbound::ExportToJson());
        if (!password.isEmpty()) object["password"] = password;
        if (auto tlsObj = tls->ExportToJson(); !tlsObj.isEmpty()) object["tls"] = tlsObj;
        if (auto transportObj = transport->ExportToJson(); !transportObj.isEmpty()) object["transport"] = transportObj;
        if (auto muxObj = multiplex->ExportToJson(); !muxObj.isEmpty()) object["multiplex"] = muxObj;
        return object;
    }
    BuildResult Trojan::Build()
    {
        if (IsXray()) {
            return {QJsonObject{{"type", "socks"}, {"server", "127.0.0.1"}}, ""};
        }
        QJsonObject object;
        object["type"] = "trojan";
        mergeJsonObjects(object, outbound::Build().object);
        if (!password.isEmpty()) object["password"] = password;
        if (auto tlsObj = tls->Build().object; !tlsObj.isEmpty()) object["tls"] = tlsObj;
        if (auto transportObj = transport->Build().object; !transportObj.isEmpty()) object["transport"] = transportObj;
        if (auto muxObj = multiplex->Build().object; !muxObj.isEmpty()) object["multiplex"] = muxObj;
        return {object, ""};
    }

    BuildResult Trojan::BuildXray()
    {
        QJsonObject sb;
        sb["type"] = "trojan";
        mergeJsonObjects(sb, outbound::ExportToJson());
        if (!password.isEmpty()) sb["password"] = password;
        if (auto tlsObj = tls->Build().object; !tlsObj.isEmpty()) sb["tls"] = tlsObj;
        auto transportObj = transport->ExportFullJson();
        if (!transportObj.isEmpty()) sb["transport"] = transportObj;
        return {singBoxOutboundToXray(sb), ""};
    }

    QString Trojan::DisplayType()
    {
        return "Trojan";
    }
}


