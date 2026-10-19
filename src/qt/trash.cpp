// SPDX-License-Identifier: LGPL-2.1-or-later
#include "trash.h"
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QSet>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace {
bool ownedDirectory(const QString &path) {
    struct stat st{};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0
        && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && !(st.st_mode & (S_IWGRP | S_IWOTH));
}
bool usable(const TrashLocation &location) {
    return ownedDirectory(location.directory) && ownedDirectory(location.directory + "/files")
        && ownedDirectory(location.directory + "/info");
}
Entry readEntry(const TrashLocation &location, const QFileInfo &file) {
    Entry entry;
    entry.path = file.absoluteFilePath(); entry.name = file.fileName();
    entry.trashDirectory = location.directory; entry.trashTop = location.topDirectory; entry.trashHome = location.home;
    entry.directory = file.isDir(); entry.symlink = file.isSymLink();
    entry.bytes = entry.directory ? 0 : qMax<qint64>(0, file.size());
    entry.modifiedMs = file.lastModified().toMSecsSinceEpoch();
    QFile metadata(location.directory + "/info/" + file.fileName() + ".trashinfo");
    // Never follow metadata symlinks or treat an oversized/damaged record as a restore destination.
    if (QFileInfo(metadata).isSymLink() || !QFileInfo(metadata).isFile() || !metadata.open(QIODevice::ReadOnly) || metadata.size() > 64 * 1024) return entry;
    if (metadata.readLine().trimmed() != "[Trash Info]") return entry;
    QByteArray path, deleted;
    bool havePath = false, haveDate = false;
    while (!metadata.atEnd()) {
        const auto line = metadata.readLine().trimmed();
        if (!havePath && line.startsWith("Path=")) { path = line.mid(5); havePath = true; }
        if (!haveDate && line.startsWith("DeletionDate=")) { deleted = line.mid(13); haveDate = true; }
    }
    const QString decoded = QFile::decodeName(QByteArray::fromPercentEncoding(path));
    const bool absolute = decoded.startsWith('/');
    if (decoded.isEmpty() || decoded.contains(QChar(0)) || decoded.split('/').contains("..")
        || (!location.home && absolute)) return entry;
    const QString original = QDir::cleanPath(absolute ? decoded : location.topDirectory + '/' + decoded);
    // Do not allow restoring over a trash store or the filesystem root itself.
    if (original == "/" || original == location.directory || original.startsWith(location.directory + '/')) return entry;
    entry.originalPath = original;
    entry.name = QFileInfo(original).fileName();
    entry.deletionDate = QDateTime::fromString(QString::fromUtf8(deleted), Qt::ISODate).toString(Qt::ISODate);
    return entry;
}
QString validate(const Entry &entry) {
    const TrashLocation location{entry.trashDirectory, entry.trashTop, entry.trashHome};
    if (!usable(location) || QFileInfo(entry.path).dir().absolutePath() != location.directory + "/files")
        return "Trash location is no longer available. Refresh and try again.";
    if (!QFileInfo::exists(entry.path) && !QFileInfo(entry.path).isSymLink()) return "Item is no longer in Trash. Refresh and try again.";
    const auto current = readEntry(location, QFileInfo(entry.path));
    if (current.originalPath != entry.originalPath || current.deletionDate != entry.deletionDate
        || current.modifiedMs != entry.modifiedMs || current.symlink != entry.symlink)
        return "Trash item changed. Refresh and try again.";
    return {};
}
}
QList<TrashLocation> TrashStore::locations() {
    QList<TrashLocation> result;
    QSet<QString> seen;
    auto add = [&](const QString &directory, const QString &top, bool home) {
        TrashLocation location{QDir::cleanPath(directory), top, home};
        if (usable(location) && !seen.contains(location.directory)) {
            result << location; seen.insert(location.directory);
        }
    };
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    add(data + "/Trash", data, true);
    const QString uid = QString::number(::getuid());
    for (const auto &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady()) continue;
        const auto top = volume.rootPath();
        const QString shared = QDir(top).filePath(".Trash");
        struct stat st{};
        if (::lstat(QFile::encodeName(shared).constData(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX))
            add(shared + '/' + uid, top, false);
        add(QDir(top).filePath(".Trash-" + uid), top, false);
    }
    return result;
}
QStringList TrashStore::watchPaths() {
    QStringList paths;
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    // A not-yet-created home Trash also needs to become visible when another application creates it.
    for (const QString &path : {data, data + "/Trash"}) if (QFileInfo(path).isDir()) paths << path;
    for (const auto &location : locations()) paths << location.directory << location.directory + "/files" << location.directory + "/info";
    paths.removeDuplicates();
    return paths;
}
ScanResult TrashStore::scan(const QList<TrashLocation> &locations, const std::shared_ptr<std::atomic_bool> &cancel, int limit) {
    ScanResult result;
    QMimeDatabase mime;
    bool damaged = false;
    for (const auto &location : locations) {
        if (!usable(location)) continue;
        if (!QFileInfo(location.directory + "/files").isReadable()) {
            result.warning += " Cannot read Trash: " + location.directory;
            continue;
        }
        QDirIterator files(location.directory + "/files", QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (files.hasNext() && !*cancel) {
            files.next();
            if (limit >= 0 && result.entries.size() >= limit) {
                result.warning = "Trash view limited to " + QString::number(limit) + " items. Empty Trash applies to all items on available volumes.";
                return result;
            }
            auto entry = readEntry(location, files.fileInfo());
            damaged |= entry.originalPath.isEmpty();
            entry.mime = entry.directory ? "inode/directory" : mime.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension).name();
            result.entries << entry;
        }
        if (*cancel) break;
    }
    if (damaged) result.warning += "Some items have missing or invalid Trash information. They can be deleted, but cannot be restored automatically.";
    return result;
}
QString TrashStore::restore(const Entry &entry) {
    const auto error = validate(entry); if (!error.isEmpty()) return error;
    if (entry.originalPath.isEmpty()) return "Original location is missing or invalid; this item cannot be restored automatically.";
    if (!QFileInfo(QFileInfo(entry.originalPath).absolutePath()).isDir())
        return "Original parent folder is missing. Recreate it before restoring: " + QFileInfo(entry.originalPath).absolutePath();
    // Linux renameat2 is atomic, preserves directories/symlinks and never overwrites a destination.
    if (::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(entry.path).constData(), AT_FDCWD,
                  QFile::encodeName(entry.originalPath).constData(), RENAME_NOREPLACE) != 0)
        return "Cannot restore to " + entry.originalPath + ": " + QString::fromLocal8Bit(std::strerror(errno));
    if (!QFile::remove(entry.trashDirectory + "/info/" + QFileInfo(entry.path).fileName() + ".trashinfo"))
        return "Item restored, but its stale Trash information could not be removed.";
    return {};
}
QString TrashStore::erase(const Entry &entry) {
    const auto error = validate(entry); if (!error.isEmpty()) return error;
    const QFileInfo file(entry.path);
    const bool ok = file.isDir() && !file.isSymLink() ? QDir(entry.path).removeRecursively() : QFile::remove(entry.path);
    if (!ok) return "Cannot permanently delete: " + entry.name;
    const QString info = entry.trashDirectory + "/info/" + file.fileName() + ".trashinfo";
    if ((QFileInfo::exists(info) || QFileInfo(info).isSymLink()) && !QFile::remove(info)) return "Item deleted, but Trash information could not be removed: " + info;
    return {};
}
QString TrashStore::empty(const QList<TrashLocation> &locations) {
    const auto snapshot = scan(locations, std::make_shared<std::atomic_bool>(false), -1);
    QStringList errors;
    // Damaged records can still be erased; unreadable stores must be reported.
    for (const auto &location : locations)
        if (!usable(location) || !QFileInfo(location.directory + "/files").isReadable())
            errors << "Cannot read Trash: " + location.directory;
    for (const auto &entry : snapshot.entries) {
        const auto error = erase(entry); if (!error.isEmpty()) errors << error;
    }
    return errors.join('\n');
}
