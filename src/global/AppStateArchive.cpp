#include "include/global/AppStateArchive.hpp"

#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace {
constexpr quint32 kMagic = 0x54525354; // TRST
constexpr quint32 kVersion = 1;
constexpr quint8 kEntryEnd = 0;
constexpr quint8 kEntryDir = 1;
constexpr quint8 kEntryFile = 2;

bool ClearDirectory(const QString& dirPath, QString* error) {
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error) *error = QString("Cannot create target directory: %1").arg(dirPath);
        return false;
    }

    const auto entries = dir.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name
    );

    for (const auto& entry : entries) {
        if (entry.isDir()) {
            QDir child(entry.absoluteFilePath());
            if (!child.removeRecursively()) {
                if (error) *error = QString("Cannot remove directory: %1").arg(entry.absoluteFilePath());
                return false;
            }
        } else {
            if (!QFile::remove(entry.absoluteFilePath())) {
                if (error) *error = QString("Cannot remove file: %1").arg(entry.absoluteFilePath());
                return false;
            }
        }
    }

    return true;
}

QString RelativePathFromBase(const QString& baseDir, const QString& absolutePath) {
    const QDir base(baseDir);
    return QDir::cleanPath(base.relativeFilePath(absolutePath));
}
} // namespace

namespace AppStateArchive {

bool CreateArchive(const QString& sourceDir, const QString& archivePath, QString* error) {
    QDir root(sourceDir);
    if (!root.exists()) {
        if (error) *error = QString("Source directory does not exist: %1").arg(sourceDir);
        return false;
    }

    QFile outFile(archivePath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QString("Cannot open archive for writing: %1").arg(archivePath);
        return false;
    }

    QDataStream out(&outFile);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic << kVersion;

    QDirIterator it(sourceDir,
                    QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        const QString absPath = it.next();
        const QFileInfo info(absPath);
        const QString relPath = RelativePathFromBase(sourceDir, absPath);
        if (relPath.isEmpty() || relPath == ".") continue;

        if (info.isSymLink()) {
            continue;
        }

        if (info.isDir()) {
            out << kEntryDir << relPath;
            continue;
        }

        if (!info.isFile()) {
            continue;
        }

        QFile inFile(absPath);
        if (!inFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QString("Cannot read file: %1").arg(absPath);
            return false;
        }

        const QByteArray data = inFile.readAll();
        out << kEntryFile << relPath << data;
    }

    out << kEntryEnd;

    if (out.status() != QDataStream::Ok) {
        if (error) *error = QString("Failed while writing archive: %1").arg(archivePath);
        return false;
    }

    return true;
}

bool RestoreArchive(const QString& archivePath, const QString& targetDir, QString* error) {
    QFile inFile(archivePath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("Cannot open archive for reading: %1").arg(archivePath);
        return false;
    }

    QDataStream in(&inFile);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint32 version = 0;
    in >> magic >> version;
    if (magic != kMagic || version != kVersion) {
        if (error) *error = QString("Invalid archive format: %1").arg(archivePath);
        return false;
    }

    if (!ClearDirectory(targetDir, error)) {
        return false;
    }

    QDir target(targetDir);
    while (true) {
        quint8 entryType = kEntryEnd;
        in >> entryType;
        if (in.status() != QDataStream::Ok) {
            if (error) *error = QString("Corrupted archive stream: %1").arg(archivePath);
            return false;
        }
        if (entryType == kEntryEnd) break;

        QString relPath;
        in >> relPath;
        if (relPath.isEmpty() || relPath.startsWith("../") || relPath.contains("/../")) {
            if (error) *error = QString("Unsafe path in archive: %1").arg(relPath);
            return false;
        }

        const QString absPath = target.absoluteFilePath(relPath);

        if (entryType == kEntryDir) {
            if (!QDir().mkpath(absPath)) {
                if (error) *error = QString("Cannot create directory: %1").arg(absPath);
                return false;
            }
            continue;
        }

        if (entryType == kEntryFile) {
            QByteArray data;
            in >> data;

            const QFileInfo fi(absPath);
            if (!QDir().mkpath(fi.absolutePath())) {
                if (error) *error = QString("Cannot create parent directory: %1").arg(fi.absolutePath());
                return false;
            }

            QFile outFile(absPath);
            if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                if (error) *error = QString("Cannot write file: %1").arg(absPath);
                return false;
            }
            if (outFile.write(data) != data.size()) {
                if (error) *error = QString("Failed writing file data: %1").arg(absPath);
                return false;
            }
            outFile.close();
            continue;
        }

        if (error) *error = QString("Unknown entry type in archive: %1").arg(entryType);
        return false;
    }

    return true;
}

} // namespace AppStateArchive
