#include "include/sys/linux/systemChecks.h"

#include <QFile>

bool isSystemdResolvedDefaultResolver() {
    if (QFile file("/etc/resolv.conf"); file.open(QIODevice::ReadOnly)) {
        while (!file.atEnd()) {
            QByteArray line = file.readLine().trimmed();
            if (line.startsWith("nameserver") && line.contains("127.0.0.53")) {
                return true;
            }
        }
    }
    return false;
}
