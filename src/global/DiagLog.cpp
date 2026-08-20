#include "include/global/DiagLog.h"
#include "include/global/Utils.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

namespace DiagLog {
    namespace {
        QMutex g_mu;
        QFile g_file;

        QString logPath() {
            const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            return QDir(desktop.isEmpty() ? QDir::homePath() + "/Desktop" : desktop).filePath("logs.txt");
        }

        void ensureOpenLocked(bool truncate) {
            const QString path = logPath();
            if (g_file.isOpen() && g_file.fileName() == path && !truncate) return;
            if (g_file.isOpen()) g_file.close();
            g_file.setFileName(path);
            QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
            mode |= truncate ? QIODevice::Truncate : QIODevice::Append;
            if (!g_file.open(mode)) return;
        }

        QString stamp() {
            return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        }
    }

    QString FilePath() {
        return logPath();
    }

    void ResetSession(const QString &title) {
        QMutexLocker lock(&g_mu);
        ensureOpenLocked(true);
        if (!g_file.isOpen()) return;
        const QString header = stamp() + " [UI] ===== " + title + " ===== path=" + logPath() + "\n";
        g_file.write(header.toUtf8());
        g_file.flush();
        lock.unlock();
        if (MW_show_log) MW_show_log("Diagnostic test log: " + logPath());
    }

    void Write(const QString &message) {
        QMutexLocker lock(&g_mu);
        ensureOpenLocked(false);
        if (!g_file.isOpen()) return;
        QString line = stamp();
        line += " [UI] [";
        line += QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16);
        line += "] ";
        line += message;
        line += '\n';
        g_file.write(line.toUtf8());
        g_file.flush();
    }
}
