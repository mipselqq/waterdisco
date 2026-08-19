#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>

#include "include/global/AppStateArchive.hpp"

namespace {
void WriteFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(data) == data.size());
}

QByteArray ReadFile(const QString& path) {
    QFile file(path);
    assert(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
}

int main() {
    QTemporaryDir root;
    assert(root.isValid());
    const QDir work(root.path());
    const QString config = work.absoluteFilePath("config");
    const QString snapshots = work.absoluteFilePath("snapshots");
    assert(QDir().mkpath(config));
    assert(QDir().mkpath(snapshots));
    WriteFile(QDir(config).absoluteFilePath("throne.db"), "old-db");
    WriteFile(QDir(config).absoluteFilePath("throne_stats.db"), "old-stats");
    WriteFile(QDir(config).absoluteFilePath("keep.txt"), "keep");
    assert(QDir().mkpath(QDir(config).absoluteFilePath("temp")));
    WriteFile(QDir(config).absoluteFilePath("temp/runtime.log"), "do-not-export");
    WriteFile(QDir(snapshots).absoluteFilePath("throne.db"), "snapshot-db");
    WriteFile(QDir(snapshots).absoluteFilePath("throne_stats.db"), "snapshot-stats");

    const QString archive = work.absoluteFilePath("state.thronestate");
    QString error;
    assert(AppStateArchive::CreateArchive(config, archive, snapshots, &error));
    assert(AppStateArchive::StageImport(archive, work, &error));
    assert(ReadFile(QDir(config).absoluteFilePath("throne.db")) == "old-db");
    assert(AppStateArchive::ApplyPendingImport(work, &error));
    assert(ReadFile(QDir(config).absoluteFilePath("throne.db")) == "snapshot-db");
    assert(ReadFile(QDir(config).absoluteFilePath("throne_stats.db")) == "snapshot-stats");
    assert(ReadFile(QDir(config).absoluteFilePath("keep.txt")) == "keep");
    assert(!QFile::exists(QDir(config).absoluteFilePath("temp/runtime.log")));
    return 0;
}
