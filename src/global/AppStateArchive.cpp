#include "include/global/AppStateArchive.hpp"

#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace {
constexpr quint32 kMagic = 0x54525354; // TRST, compatible with the original fork.
constexpr quint32 kCurrentVersion = 2;
constexpr quint32 kLegacyVersion = 1;
constexpr quint8 kEntryEnd = 0;
constexpr quint8 kEntryDir = 1;
constexpr quint8 kEntryFile = 2;
constexpr qint64 kMaxArchiveBytes = 1024LL * 1024 * 1024;
constexpr qint64 kMaxEntryBytes = 256LL * 1024 * 1024;
constexpr int kMaxEntries = 100000;
constexpr auto kPendingMarker = ".throne-import-pending";
constexpr auto kStagingPrefix = ".throne-import-staging-";
constexpr auto kBackupDir = ".throne-import-backup";

struct Entry {
    quint8 type;
    QString path;
    QByteArray data;
};

void SetError(QString* error, const QString& text) {
    if (error) *error = text;
}

bool IsSafeRelativePath(const QString& path) {
    QString normalized = QDir::cleanPath(path).replace('\\', '/');
    return !normalized.isEmpty() && normalized != "." && !QDir::isAbsolutePath(normalized)
        && normalized != ".." && !normalized.startsWith("../")
        && !normalized.contains("/../");
}

bool IsExcluded(const QString& relPath, const QString& sourcePath, const QString& archivePath) {
    const QString normalized = QDir::cleanPath(relPath).replace('\\', '/');
    const QString firstPart = normalized.section('/', 0, 0);
    if (firstPart == "temp" || firstPart == "logs" || firstPart.startsWith(".throne-import-")) return true;
    const QString leaf = QFileInfo(normalized).fileName();
    if (leaf.endsWith(".log", Qt::CaseInsensitive) || leaf.endsWith(".lock", Qt::CaseInsensitive)) return true;
    if (leaf == "throne.db-wal" || leaf == "throne.db-shm"
        || leaf == "throne_stats.db-wal" || leaf == "throne_stats.db-shm") return true;
    return QFileInfo(archivePath).absoluteFilePath() == QFileInfo(sourcePath).absoluteFilePath();
}

bool ReadEntries(const QString& archivePath, QList<Entry>* entries, QString* error) {
    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.exists() || archiveInfo.size() <= 0 || archiveInfo.size() > kMaxArchiveBytes) {
        SetError(error, QObject::tr("Archive size is invalid"));
        return false;
    }

    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) {
        SetError(error, QObject::tr("Cannot open archive: %1").arg(archivePath));
        return false;
    }
    QDataStream stream(&input);
    stream.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint32 version = 0;
    stream >> magic >> version;
    if (stream.status() != QDataStream::Ok || magic != kMagic
        || (version != kLegacyVersion && version != kCurrentVersion)) {
        SetError(error, QObject::tr("Invalid Waterdisco state archive"));
        return false;
    }

    QSet<QString> seen;
    qint64 totalBytes = 0;
    while (true) {
        quint8 type = kEntryEnd;
        stream >> type;
        if (stream.status() != QDataStream::Ok) {
            SetError(error, QObject::tr("Archive is truncated"));
            return false;
        }
        if (type == kEntryEnd) break;
        if (type != kEntryDir && type != kEntryFile) {
            SetError(error, QObject::tr("Archive contains an unknown entry type"));
            return false;
        }

        Entry entry{type, {}, {}};
        stream >> entry.path;
        if (stream.status() != QDataStream::Ok || !IsSafeRelativePath(entry.path)) {
            SetError(error, QObject::tr("Archive contains an unsafe path"));
            return false;
        }
        entry.path = QDir::cleanPath(entry.path).replace('\\', '/');
        if (seen.contains(entry.path) || seen.size() >= kMaxEntries) {
            SetError(error, QObject::tr("Archive contains duplicate or too many entries"));
            return false;
        }
        seen.insert(entry.path);

        if (type == kEntryFile) {
            stream >> entry.data;
            if (stream.status() != QDataStream::Ok || entry.data.size() > kMaxEntryBytes) {
                SetError(error, QObject::tr("Archive contains an invalid file entry"));
                return false;
            }
            totalBytes += entry.data.size();
            if (totalBytes > kMaxArchiveBytes) {
                SetError(error, QObject::tr("Archive expands beyond the allowed size"));
                return false;
            }
        }
        entries->append(std::move(entry));
    }
    return true;
}

bool WriteEntries(const QList<Entry>& entries, const QString& archivePath, QString* error) {
    QSaveFile output(archivePath);
    if (!output.open(QIODevice::WriteOnly)) {
        SetError(error, QObject::tr("Cannot create archive: %1").arg(archivePath));
        return false;
    }
    QDataStream stream(&output);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kMagic << kCurrentVersion;
    for (const auto& entry : entries) {
        stream << entry.type << entry.path;
        if (entry.type == kEntryFile) stream << entry.data;
    }
    stream << kEntryEnd;
    if (stream.status() != QDataStream::Ok || !output.commit()) {
        SetError(error, QObject::tr("Failed to write archive: %1").arg(archivePath));
        return false;
    }
    return true;
}

bool ExtractEntries(const QList<Entry>& entries, const QString& directory, QString* error) {
    if (!QDir().mkpath(directory)) {
        SetError(error, QObject::tr("Cannot create import staging directory"));
        return false;
    }
    QDir root(directory);
    for (const auto& entry : entries) {
        const QString path = root.absoluteFilePath(entry.path);
        if (entry.type == kEntryDir) {
            if (!QDir().mkpath(path)) {
                SetError(error, QObject::tr("Cannot create imported directory"));
                return false;
            }
            continue;
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            SetError(error, QObject::tr("Cannot create imported file directory"));
            return false;
        }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(entry.data) != entry.data.size() || !output.commit()) {
            SetError(error, QObject::tr("Cannot write imported file"));
            return false;
        }
    }
    if (!QFileInfo::exists(root.absoluteFilePath("throne.db"))) {
        SetError(error, QObject::tr("Archive does not contain throne.db"));
        return false;
    }
    return true;
}

bool IsContainedStagingDirectory(const QDir& workingDir, const QString& path) {
    const QString cleanRoot = QDir::cleanPath(workingDir.absolutePath()) + '/';
    const QFileInfo info(path);
    return QDir::cleanPath(info.absoluteFilePath()).startsWith(cleanRoot)
        && info.fileName().startsWith(kStagingPrefix);
}
} // namespace

namespace AppStateArchive {

bool CreateArchive(const QString& sourceDir, const QString& archivePath,
                   const QString& snapshotDir, QString* error) {
    QDir source(sourceDir);
    if (!source.exists()) {
        SetError(error, QObject::tr("Configuration directory does not exist"));
        return false;
    }

    QList<Entry> entries;
    QDirIterator iterator(source.absolutePath(), QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
                           QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString sourcePath = iterator.next();
        const QFileInfo info(sourcePath);
        QString relativePath = source.relativeFilePath(sourcePath);
        if (!IsSafeRelativePath(relativePath) || IsExcluded(relativePath, sourcePath, archivePath) || info.isSymLink()) continue;

        if (info.isDir()) {
            entries.append({kEntryDir, QDir::cleanPath(relativePath), {}});
            continue;
        }
        if (!info.isFile() || info.size() > kMaxEntryBytes) {
            if (info.isFile()) SetError(error, QObject::tr("A configuration file is too large to export"));
            if (error && !error->isEmpty()) return false;
            continue;
        }

        const QString leaf = QFileInfo(relativePath).fileName();
        const QString snapshotPath = (leaf == "throne.db" || leaf == "throne_stats.db")
            ? QDir(snapshotDir).absoluteFilePath(leaf) : sourcePath;
        QFile file(snapshotPath);
        if (!file.open(QIODevice::ReadOnly)) {
            SetError(error, QObject::tr("Cannot read configuration file: %1").arg(relativePath));
            return false;
        }
        const QByteArray data = file.readAll();
        if (data.size() > kMaxEntryBytes) {
            SetError(error, QObject::tr("A configuration file is too large to export"));
            return false;
        }
        entries.append({kEntryFile, QDir::cleanPath(relativePath), data});
    }
    return WriteEntries(entries, archivePath, error);
}

bool StageImport(const QString& archivePath, const QDir& workingDir, QString* error) {
    QList<Entry> entries;
    if (!ReadEntries(archivePath, &entries, error)) return false;

    const QString stagingPath = workingDir.absoluteFilePath(
        QString::fromLatin1(kStagingPrefix) + QUuid::createUuid().toString(QUuid::Id128));
    if (!ExtractEntries(entries, stagingPath, error)) {
        QDir(stagingPath).removeRecursively();
        return false;
    }

    QSaveFile marker(workingDir.absoluteFilePath(kPendingMarker));
    if (!marker.open(QIODevice::WriteOnly) || marker.write(stagingPath.toUtf8()) < 0 || !marker.commit()) {
        QDir(stagingPath).removeRecursively();
        SetError(error, QObject::tr("Cannot stage the import restart"));
        return false;
    }
    return true;
}

bool ApplyPendingImport(const QDir& workingDir, QString* error) {
    QFile marker(workingDir.absoluteFilePath(kPendingMarker));
    if (!marker.exists()) return true;
    if (!marker.open(QIODevice::ReadOnly)) {
        SetError(error, QObject::tr("Cannot read pending import marker"));
        return false;
    }
    const QString stagingPath = QString::fromUtf8(marker.readAll()).trimmed();
    marker.close();
    if (!IsContainedStagingDirectory(workingDir, stagingPath)
        || !QFileInfo::exists(QDir(stagingPath).absoluteFilePath("throne.db"))) {
        marker.remove();
        SetError(error, QObject::tr("Pending import staging directory is invalid"));
        return false;
    }

    const QString configPath = workingDir.absoluteFilePath("config");
    const QString backupPath = workingDir.absoluteFilePath(kBackupDir);
    QDir(backupPath).removeRecursively();
    const bool hadConfig = QDir(configPath).exists();
    if (hadConfig && !QDir().rename(configPath, backupPath)) {
        SetError(error, QObject::tr("Cannot prepare the current configuration for import"));
        return false;
    }
    if (!QDir().rename(stagingPath, configPath)) {
        if (hadConfig) QDir().rename(backupPath, configPath);
        SetError(error, QObject::tr("Cannot activate imported configuration"));
        return false;
    }
    marker.remove();
    QDir(backupPath).removeRecursively();
    return true;
}

} // namespace AppStateArchive
