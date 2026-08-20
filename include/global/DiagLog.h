#pragma once

#include <QString>

// Temporary diagnostic dump for speedtest/skip/parallelism. Writes to
// ~/Desktop/logs.txt so a full-list run can be inspected after the fact.
namespace DiagLog {
    QString FilePath();
    void ResetSession(const QString &title);
    void Write(const QString &message);
}

#define DIAG_LOG(msg) ::DiagLog::Write(msg)
