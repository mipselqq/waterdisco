#pragma once

#ifndef Q_MOC_RUN
#include <core/server/gen/libcore.pb.h>
#endif
#include <QString>

class QLocalSocket;

namespace API {
    class Client {
    public:
        Client();

        ~Client();

        // Adopt a freshly connected socket, replacing any previous
        // connection. The Client itself is long-lived and never recreated.
        void Reconnect(QLocalSocket *socket);

        // QString returns is error string

        QString Start(bool *rpcOK, const libcore::LoadConfigReq &request);

        QString Stop(bool *rpcOK);

        libcore::QueryStatsResp QueryStats();

        // coreError (optional): on RPC failure, receives the core's error message
        // so callers can react to it (e.g. missing Xray geo assets) rather than
        // silently dropping the failed test.
        libcore::TestResp Test(bool *rpcOK, const libcore::TestReq &request, QString *coreError = nullptr);

        void StopTests(bool *rpcOK);

        void FailInFlightCalls();

        libcore::QueryURLTestResponse QueryURLTest(bool *rpcOK);

        libcore::IPTestResp IPTest(bool *rpcOK, const libcore::IPTestRequest &request, QString *coreError = nullptr);

        libcore::QueryIPTestResponse QueryIPTest(bool *rpcOK);

        QString SetSystemDNS(bool *rpcOK, bool clear) const;

        [[nodiscard]] libcore::QueryConnectionsResp QueryConnections() const;

        // isXray selects the validating core: false (default) validates a
        // sing-box config, true validates an Xray-format config.
        QString CheckConfig(bool *rpcOK, const QString& config, bool isXray = false) const;

        bool IsPrivileged(bool *rpcOK) const;

        libcore::SpeedTestResponse SpeedTest(bool *rpcOK, const libcore::SpeedTestRequest &request, QString *coreError = nullptr);

        libcore::QuerySpeedTestResponse QueryCurrentSpeedTests(bool *rpcOK);

        libcore::QueryCountryTestResponse QueryCountryTestResults(bool *rpcOK);

        libcore::GenWgKeyPairResponse GenWgKeyPair(bool *rpcOK);

        // Empty name = the OS has no default route. A local, censorship-proof
        // way to tell "my network died" from "the servers died".
        [[nodiscard]] libcore::GetDefaultInterfaceResponse GetDefaultInterface(bool *rpcOK) const;

        // Idempotent snapshot of every running auto-selector group: it clears no
        // core-side counters, so polling it alongside QueryStats is safe.
        [[nodiscard]] libcore::QueryAutoSelectorsResponse QueryAutoSelectors(bool *rpcOK) const;

        // action: "recheck" forces a full sweep now, "select" pins the group to
        // member. An empty tag targets every auto-selector group.
        QString AutoSelectorAction(bool *rpcOK, const QString &tag, const QString &action,
                                   const QString &member = {}) const;

    private:
        class LocalSocketChannel;
        std::unique_ptr<LocalSocketChannel> channel;
    };

    inline Client *defaultClient;
} // namespace API
