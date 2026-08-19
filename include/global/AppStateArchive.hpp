#pragma once

#include <QString>

class QDir;

namespace AppStateArchive {

// Writes the complete persistent configuration. snapshotDir must contain
// WAL-safe copies of throne.db and throne_stats.db.
bool CreateArchive(const QString& sourceDir, const QString& archivePath,
                   const QString& snapshotDir, QString* error = nullptr);

// Validates and extracts an archive into a new staging directory beside the
// configuration directory. The live configuration is not touched here.
bool StageImport(const QString& archivePath, const QDir& workingDir, QString* error = nullptr);

// Called before the database is opened. Atomically replaces config/ with a
// previously validated staging directory and restores the old directory if a
// rename fails.
bool ApplyPendingImport(const QDir& workingDir, QString* error = nullptr);

} // namespace AppStateArchive
