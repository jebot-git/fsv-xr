// SPDX-License-Identifier: LGPL-2.1-or-later
#include "archives.h"
#include "filesystem.h"
#include <archive.h>
#include <archive_entry.h>
#include <QCache>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMimeDatabase>
#include <QMutex>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <limits>

namespace {
constexpr qint64 memberLimit = 64LL * 1024 * 1024, cacheLimit = 256LL * 1024 * 1024;
bool cancelled(const std::shared_ptr<std::atomic_bool> &cancel) { return cancel && *cancel; }
bool safeName(const QString &name) {
    return !name.isEmpty() && !QDir::isAbsolutePath(name) && !name.contains('\\') && !name.contains(':')
        && !name.contains(QChar(0)) && !name.split('/').contains("..");
}
QString entryName(archive_entry *entry) {
    const char *name = archive_entry_pathname_utf8(entry);
    if (!name) name = archive_entry_pathname(entry);
    return QString::fromUtf8(name ? name : "");
}
QString failure(archive *a) {
    return "Cannot read archive: " + QString::fromLocal8Bit(archive_error_string(a) ? archive_error_string(a) : "invalid archive");
}
QString fingerprint(const QString &file) {
    const QFileInfo info(file);
    return file + '\n' + QString::number(info.size()) + ':' + QString::number(info.lastModified().toMSecsSinceEpoch())
        + ':' + QString::number(info.metadataChangeTime().toMSecsSinceEpoch());
}
// A cancellable reader also checks cancellation while libarchive skips data in tar streams.
struct Reader {
    QFile file;
    QByteArray buffer = QByteArray(65536, Qt::Uninitialized);
    std::shared_ptr<std::atomic_bool> cancel;
    std::unique_ptr<archive, decltype(&archive_read_free)> handle{archive_read_new(), archive_read_free};
    Reader(const QString &path, const std::shared_ptr<std::atomic_bool> &token) : file(path), cancel(token) {}
    static la_ssize_t read(archive *a, void *context, const void **data) {
        auto &r = *static_cast<Reader *>(context);
        if (cancelled(r.cancel)) { archive_set_error(a, ECANCELED, "Cancelled"); return -1; }
        const auto size = r.file.read(r.buffer.data(), r.buffer.size());
        if (size < 0) archive_set_error(a, EIO, "Cannot read archive file");
        *data = r.buffer.constData(); return size;
    }
    static la_int64_t seek(archive *a, void *context, la_int64_t offset, int whence) {
        auto &r = *static_cast<Reader *>(context);
        const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? r.file.pos() : r.file.size();
        if (cancelled(r.cancel) || base < 0 || offset < -base || offset > std::numeric_limits<qint64>::max() - base
            || !r.file.seek(base + offset)) {
            archive_set_error(a, EIO, "Cannot seek archive"); return -1;
        }
        return r.file.pos();
    }
    QString open() {
        if (!file.open(QIODevice::ReadOnly)) return "Cannot open archive: " + file.fileName();
        archive_read_support_filter_all(handle.get());
        archive_read_support_format_tar(handle.get());
        archive_read_support_format_zip(handle.get());
        archive_read_set_seek_callback(handle.get(), seek);
        if (archive_read_open(handle.get(), this, nullptr, read, nullptr) != ARCHIVE_OK) return failure(handle.get());
        return {};
    }
};
struct Member {
    QString name;
    qint64 bytes = 0, modifiedMs = 0;
    bool directory = false, blocked = false, explicitEntry = false;
};
struct Listing { QMap<QString, Member> entries; QString error; };
QMutex indexMutex;
QCache<QString, Listing> indexes(32768); // Approximate KiB, bounded across all archives.
Listing indexArchive(const QString &file, const std::shared_ptr<std::atomic_bool> &cancel, bool reload) {
    const QString key = fingerprint(file);
    {
        QMutexLocker lock(&indexMutex);
        if (!reload) if (auto *cached = indexes.object(key)) return *cached;
        if (reload) indexes.remove(key);
    }
    Listing result;
    Reader reader(file, cancel);
    result.error = reader.open();
    if (!result.error.isEmpty()) return result;
    archive *a = reader.handle.get();
    archive_entry *entry = nullptr;
    int status, count = 0;
    qint64 nameBytes = 0, total = 0;
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK && !cancelled(cancel)) {
        const QString raw = entryName(entry), name = QDir::cleanPath(raw);
        nameBytes += raw.size() * 2;
        const qint64 bytes = qMax<qint64>(0, archive_entry_size(entry));
        const qint64 dataLimit = 10LL * 1024 * 1024 * 1024;
        if (++count > 100000 || nameBytes > 16 * 1024 * 1024 || bytes > dataLimit - total) {
            result.error = "Archive exceeds browsing limits (100,000 entries, 16 MiB of names, 10 GiB of data)."; break;
        }
        total += bytes;
        if (!safeName(raw) || raw.size() > 4096 || raw.count('/') > 128) {
            result.error = "Unsafe or excessively deep archive path: " + raw; break;
        }
        const auto type = archive_entry_filetype(entry);
        if (name == "." && type == AE_IFDIR) continue;
        if (name == ".") { result.error = "Invalid archive member path."; break; }
        const auto seconds = archive_entry_mtime(entry);
        const qint64 modifiedMs = seconds >= 0 && seconds <= std::numeric_limits<qint64>::max() / 1000 ? seconds * 1000 : 0;
        Member item{name, bytes, modifiedMs,
            type == AE_IFDIR, bool(archive_entry_symlink(entry) || archive_entry_hardlink(entry)
                || (type != AE_IFDIR && type != AE_IFREG)), true};
        if (item.blocked) item.directory = false;
        if (result.entries.contains(name)) {
            const auto &old = result.entries[name];
            if (old.explicitEntry || !item.directory) { result.error = "Duplicate archive path: " + name; break; }
        }
        result.entries.insert(name, item);
        QString parent = name.section('/', 0, -2);
        while (!parent.isEmpty()) {
            if (result.entries.contains(parent)) {
                if (!result.entries[parent].directory) result.error = "Archive file conflicts with a folder: " + parent;
                break;
            }
            nameBytes += parent.size() * 2;
            if (nameBytes > 16 * 1024 * 1024) { result.error = "Archive folder names exceed the metadata limit."; break; }
            result.entries.insert(parent, Member{parent, 0, 0, true, false, false});
            parent = parent.section('/', 0, -2);
        }
        if (result.entries.size() > 100000) result.error = "Archive has too many folders and files.";
        if (!result.error.isEmpty()) break;
    }
    if (cancelled(cancel)) return {};
    if (result.error.isEmpty() && status != ARCHIVE_EOF) result.error = failure(a);
    if (result.error.isEmpty() && fingerprint(file) != key) result.error = "Archive changed while it was being read. Refresh to try again.";
    if (result.error.isEmpty()) {
        QMutexLocker lock(&indexMutex);
        const int cost = int((nameBytes + result.entries.size() * 160 + 1023) / 1024);
        indexes.insert(key, new Listing(result), qMax(1, cost));
    }
    return result;
}
struct FileCache {
    QMutex mutex;
    QTemporaryDir directory;
    QMap<QString, QString> paths;
    qint64 bytes = 0;
};
FileCache &fileCache() { static FileCache cache; return cache; }
}

bool Archives::isLocation(const QString &path) { return QUrl(path).scheme() == "archive"; }
QString Archives::location(const QString &file, const QString &member) {
    QUrl url; url.setScheme("archive"); url.setPath(QFileInfo(file).absoluteFilePath());
    if (!member.isEmpty()) url.setFragment(member);
    return url.toString(QUrl::FullyEncoded);
}
QString Archives::source(const QString &path) { return QUrl(path).path(QUrl::FullyDecoded); }
QString Archives::member(const QString &path) { return QUrl(path).fragment(QUrl::FullyDecoded); }
bool Archives::validLocation(const QString &path) {
    const QUrl url(path);
    const QString file = source(path), name = member(path);
    const QFileInfo info(file);
    return isLocation(path) && url.isValid() && url.authority().isEmpty() && !url.hasQuery()
        && QDir::isAbsolutePath(file) && supported(file) && info.isFile() && info.isReadable()
        && (name.isEmpty() || (safeName(name) && QDir::cleanPath(name) == name && name != "."));
}
QString Archives::parent(const QString &path) {
    const QString name = member(path);
    return name.isEmpty() ? QFileInfo(source(path)).absolutePath() : location(source(path), name.section('/', 0, -2));
}
QString Archives::title(const QString &path) {
    const QString name = member(path);
    return name.isEmpty() ? QFileInfo(source(path)).fileName() : name.section('/', -1);
}
ScanResult Archives::browse(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel, bool reload) {
    ScanResult result;
    if (!validLocation(path)) { result.warning = "Cannot read archive location."; return result; }
    const QString file = source(path);
    QString folder = member(path);
    const auto listing = indexArchive(file, cancel, reload);
    if (cancelled(cancel)) return result;
    if (!listing.error.isEmpty()) { result.warning = listing.error; return result; }
    if (!folder.isEmpty() && !listing.entries.contains(folder)) {
        result.warning = "Archive folder no longer exists: " + folder; return result;
    }
    if (!folder.isEmpty() && !listing.entries[folder].directory) {
        result.revealPath = location(file, folder);
        folder = folder.section('/', 0, -2);
        result.rootRedirect = location(file, folder);
    }
    QMap<QString, QStringList> children;
    for (auto it = listing.entries.cbegin(); it != listing.entries.cend(); ++it)
        children[it.key().section('/', 0, -2)].append(it.key());
    bool truncated = false;
    QMimeDatabase mime;
    std::function<void(const QString &, int, int)> visit = [&](const QString &parent, int parentIndex, int depth) {
        for (const auto &name : children.value(parent)) {
            if (cancelled(cancel)) return;
            if (result.entries.size() >= 2500) { truncated = true; return; }
            const auto &item = listing.entries[name];
            Entry e;
            e.path = location(file, name); e.name = name.section('/', -1);
            e.bytes = item.directory ? 0 : item.bytes; e.modifiedMs = item.modifiedMs;
            e.directory = item.directory; e.symlink = item.blocked; e.parent = parentIndex; e.depth = depth;
            e.mime = item.blocked ? "application/x-fsvr-blocked-entry" : item.directory ? "inode/directory"
                : mime.mimeTypeForFile(e.name, QMimeDatabase::MatchExtension).name();
            const int index = result.entries.size(); result.entries.append(e);
            if (item.directory) {
                if (depth < 4) visit(name, index, depth + 1);
                else if (!children.value(name).isEmpty()) truncated = true;
            }
        }
    };
    visit(folder, -1, 0);
    for (int i = result.entries.size() - 1; i >= 0; --i)
        if (result.entries[i].parent >= 0) result.entries[result.entries[i].parent].bytes += result.entries[i].bytes;
    result.warning = "Read-only archive. Open files as temporary copies; edits are not saved to the archive.";
    if (truncated) result.warning += " View limited to 2,500 entries / five levels; open a folder to explore further.";
    return result;
}

Archives::Materialized Archives::materialize(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (!validLocation(path) || member(path).isEmpty()) return {{}, "Invalid archive member."};
    auto &cache = fileCache();
    QMutexLocker lock(&cache.mutex);
    const QString file = source(path), name = member(path), stamp = fingerprint(file);
    const QString key = location(file, name) + '\n' + stamp;
    if (cache.paths.contains(key)) return {cache.paths[key], {}};
    const auto listing = indexArchive(file, cancel, false);
    if (cancelled(cancel)) return {{}, "Cancelled."};
    if (!listing.error.isEmpty()) return {{}, listing.error};
    if (!listing.entries.contains(name)) return {{}, "Archive member no longer exists."};
    const auto &item = listing.entries[name];
    if (item.directory || item.blocked) return {{}, "Links, folders and special files cannot be opened as archive files."};
    if (item.bytes > memberLimit) return {{}, "Archive files larger than 64 MiB must be extracted first."};
    if (!cache.directory.isValid() || cache.bytes + item.bytes > cacheLimit || cache.paths.size() >= 4096)
        return {{}, "Temporary archive cache is full or unavailable. Extract the archive to open this file."};
    // Use a private, unique parent for every copy; member names never become parent paths.
    QTemporaryDir staging(cache.directory.filePath("member-XXXXXX"));
    if (!staging.isValid()) return {{}, "Cannot create temporary archive folder."};
    const QString target = staging.filePath(name.section('/', -1));
    Reader reader(file, cancel);
    QString error = reader.open(); if (!error.isEmpty()) return {{}, error};
    auto *a = reader.handle.get();
    archive_entry *entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK && !cancelled(cancel)) {
        if (QDir::cleanPath(entryName(entry)) != name) continue;
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            return {{}, "Archive member is not a regular file."};
        QFile output(target);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return {{}, "Cannot write temporary archive file."};
        output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        char buffer[65536]; qint64 total = 0; la_ssize_t bytes;
        while ((bytes = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
            total += bytes;
            if (cancelled(cancel)) return {{}, "Cancelled."};
            if (total > memberLimit || total + cache.bytes > cacheLimit) return {{}, "Archive file exceeds the temporary copy limit."};
            if (output.write(buffer, bytes) != bytes) return {{}, "Cannot write temporary archive file."};
        }
        if (bytes < 0) return {{}, failure(a)};
        if (fingerprint(file) != stamp || total != item.bytes) return {{}, "Archive changed while reading. Refresh and try again."};
        if (!output.flush()) return {{}, "Cannot finish temporary archive file."};
        staging.setAutoRemove(false); cache.paths.insert(key, target); cache.bytes += total;
        return {target, {}};
    }
    return {{}, cancelled(cancel) ? "Cancelled." : failure(a)};
}
