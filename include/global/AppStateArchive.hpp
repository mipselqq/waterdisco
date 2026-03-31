#pragma once

#include <QString>

namespace AppStateArchive {

bool CreateArchive(const QString& sourceDir, const QString& archivePath, QString* error);
bool RestoreArchive(const QString& archivePath, const QString& targetDir, QString* error);

} // namespace AppStateArchive
