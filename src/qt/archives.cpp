// SPDX-License-Identifier: LGPL-2.1-or-later
#include "archives.h"
#include <archive.h>
#include <archive_entry.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <memory>

namespace {
QString failure(archive *a) {
    return QString("Archive operation failed: ") + QString::fromLocal8Bit(archive_error_string(a) ? archive_error_string(a) : "unknown error");
}
const QStringList suffixes = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".txz", ".zip", ".tar"};
}

bool Archives::supported(const QString &path)
{
    for (const auto &suffix : suffixes) if (path.endsWith(suffix, Qt::CaseInsensitive)) return true;
    return false;
}
QString Archives::extractionName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    for (const auto &suffix : suffixes)
        if (name.endsWith(suffix, Qt::CaseInsensitive) && name.size() > suffix.size()) return name.chopped(suffix.size());
    return name + ".extracted";
}
QString Archives::extract(const QString &path)
{
    const QFileInfo source(path);
    if (!source.isFile() || !supported(path)) return "Select a supported archive.";
    const QString destination = source.dir().filePath(extractionName(path));
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) return "Destination already exists: " + destination;
    QTemporaryDir staging(source.dir().filePath(".fsvr-extract-XXXXXX"));
    if (!staging.isValid()) return "Cannot create extraction folder.";
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    archive *a = reader.get();
    archive_read_support_filter_all(a);
    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 65536) != ARCHIVE_OK) return failure(a);
    archive_entry *entry = nullptr;
    qint64 total = 0;
    int count = 0, status;
    char buffer[65536];
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (++count > 100000) return "Archive exceeds the 100,000 entry limit.";
        const char *rawName = archive_entry_pathname_utf8(entry);
        if (!rawName) rawName = archive_entry_pathname(entry);
        const QString name = QString::fromUtf8(rawName ? rawName : "");
        const QString clean = QDir::cleanPath(name);
        if (name.isEmpty() || QDir::isAbsolutePath(name) || name.contains('\\') || name.contains(':') ||
            name.split('/').contains("..") || clean == ".." || clean.startsWith("../"))
            return "Unsafe archive path: " + name;
        const auto type = archive_entry_filetype(entry);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) || (type != AE_IFREG && type != AE_IFDIR))
            return "Archive contains a link or unsupported special file: " + name;
        if (clean == ".") {
            if (type == AE_IFDIR) continue;
            return "Invalid archive file path.";
        }
        const QString target = staging.filePath(clean);
        if (type == AE_IFDIR) {
            if (!QDir().mkpath(target)) return "Cannot create folder: " + name;
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return "Cannot create parent folder: " + name;
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return "Duplicate or unwritable archive entry: " + name;
        la_ssize_t bytes;
        while ((bytes = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
            total += bytes;
            if (total > 10LL * 1024 * 1024 * 1024) return "Archive exceeds the 10 GiB extraction limit.";
            if (file.write(buffer, bytes) != bytes) return "Cannot write extracted file: " + name;
        }
        if (bytes < 0) return failure(a);
        if (!file.flush()) return "Cannot finish extracted file: " + name;
    }
    if (status != ARCHIVE_EOF || archive_read_close(a) != ARCHIVE_OK) return failure(a);
    if (!QDir().rename(staging.path(), destination)) return "Cannot publish extraction folder: " + destination;
    staging.setAutoRemove(false);
    return {};
}

QString Archives::create(const QString &source, const QString &destination)
{
    const QFileInfo info(source), output(destination);
    const bool zip = destination.endsWith(".zip", Qt::CaseInsensitive);
    if (!zip && !destination.endsWith(".tar.gz", Qt::CaseInsensitive)) return "Use a .zip or .tar.gz archive name.";
    if (!info.exists() || info.isSymLink()) return "Select a regular file or folder; links are not supported.";
    const QString canonical = info.canonicalFilePath(), parent = output.dir().canonicalPath();
    if (info.isDir() && (parent == canonical || parent.startsWith(canonical + '/'))) return "Cannot create an archive inside its source folder.";
    if (output.exists() || output.isSymLink()) return "Destination already exists: " + destination;
    QTemporaryFile temporary(output.dir().filePath(".fsvr-archive-XXXXXX"));
    if (!temporary.open()) return "Cannot create archive file.";
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    archive *a = writer.get();
    if (zip) { if (archive_write_set_format_zip(a) != ARCHIVE_OK) return failure(a); }
    else if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK || archive_write_add_filter_gzip(a) != ARCHIVE_OK) return failure(a);
    if (archive_write_open_fd(a, temporary.handle()) != ARCHIVE_OK) return failure(a);
    auto append = [&](const QString &path) -> QString {
        const QFileInfo item(path);
        if (item.isSymLink() || (!item.isDir() && !item.isFile())) return "Links and special files cannot be compressed: " + path;
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        const QByteArray name = info.dir().relativeFilePath(path).toUtf8();
        archive_entry_set_pathname_utf8(entry.get(), name.constData());
        archive_entry_set_filetype(entry.get(), item.isDir() ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry.get(), item.isDir() ? 0755 : (item.isExecutable() ? 0755 : 0644));
        archive_entry_set_size(entry.get(), item.isDir() ? 0 : item.size());
        archive_entry_set_mtime(entry.get(), item.lastModified().toSecsSinceEpoch(), 0);
        if (archive_write_header(a, entry.get()) != ARCHIVE_OK) return failure(a);
        if (!item.isDir()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return "Cannot read: " + path;
            qint64 remaining = item.size();
            while (remaining > 0) {
                const QByteArray data = file.read(qMin<qint64>(65536, remaining));
                if (data.isEmpty()) return "Source changed or could not be read: " + path;
                if (archive_write_data(a, data.constData(), data.size()) != data.size()) return failure(a);
                remaining -= data.size();
            }
        }
        return {};
    };
    QString error = append(info.absoluteFilePath());
    if (!error.isEmpty()) return error;
    if (info.isDir()) {
        QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) { error = append(it.next()); if (!error.isEmpty()) return error; }
    }
    if (archive_write_close(a) != ARCHIVE_OK) return failure(a);
    temporary.close();
    if (!QFile::rename(temporary.fileName(), destination)) return "Cannot publish archive: " + destination;
    return {};
}
