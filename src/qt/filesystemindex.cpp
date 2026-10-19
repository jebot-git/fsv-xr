// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
QString indexPath(const QString &root, const QString &directory)
{
    return directory + '/' + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
}
}
QString FileIndex::directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/fsv-xr/index-v1";
}
std::optional<ScanResult> FileIndex::load(const QString &root, const QString &directory)
{
    QFile file(indexPath(root, directory));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) return {};
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    if (object["version"].toInt() != 1 || object["root"].toString() != root || !object["entries"].isArray()) return {};
    const auto entries = object["entries"].toArray();
    if (entries.size() > 2500) return {};
    ScanResult result;
    result.warning = object["warning"].toString();
    const QString prefix = root.endsWith('/') ? root : root + '/';
    for (const auto &value : entries) {
        const auto record = value.toObject();
        Entry entry;
        entry.path = record["path"].toString();
        entry.name = QFileInfo(entry.path).fileName();
        entry.mime = record["mime"].toString();
        bool validBytes = false, validTime = false;
        entry.bytes = record["bytes"].toString().toLongLong(&validBytes);
        entry.modifiedMs = record["modified"].toString().toLongLong(&validTime);
        entry.directory = record["directory"].toBool(); entry.symlink = record["symlink"].toBool();
        entry.parent = record["parent"].toInt(-2); entry.depth = record["depth"].toInt(-1);
        if (!validBytes || !validTime || entry.bytes < 0 || entry.mime.isEmpty()
            || !entry.path.startsWith(prefix) || QDir::cleanPath(entry.path) != entry.path
            || entry.parent < -1 || entry.parent >= result.entries.size() || entry.depth < 0 || entry.depth > 4) return {};
        const QString parent = entry.parent < 0 ? root : result.entries[entry.parent].path;
        if (QFileInfo(entry.path).path() != parent || entry.depth != (entry.parent < 0 ? 0 : result.entries[entry.parent].depth + 1)
            || (entry.parent >= 0 && (!result.entries[entry.parent].directory || result.entries[entry.parent].symlink))) return {};
        result.entries << entry;
    }
    return result;
}
bool FileIndex::store(const QString &root, const ScanResult &result, const QString &directory)
{
    if (result.entries.size() > 2500 || !QDir().mkpath(directory)) return false;
    QJsonArray entries;
    for (const auto &entry : result.entries)
        entries.append(QJsonObject{{"path", entry.path}, {"mime", entry.mime}, {"bytes", QString::number(entry.bytes)},
            {"modified", QString::number(entry.modifiedMs)}, {"directory", entry.directory}, {"symlink", entry.symlink},
            {"parent", entry.parent}, {"depth", entry.depth}});
    const auto data = QJsonDocument(QJsonObject{{"version", 1}, {"root", root}, {"warning", result.warning}, {"entries", entries}}).toJson(QJsonDocument::Compact);
    if (data.size() > 8 * 1024 * 1024) return false;
    QSaveFile file(indexPath(root, directory));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(data) != data.size() || !file.commit()) return false;
    // Bound storage across visited roots. Atomic writes also permit multiple
    // application instances; an unavailable/corrupt cache is simply a miss.
    const auto files = QDir(directory).entryInfoList({"*.json"}, QDir::Files, QDir::Time);
    for (int i = 64; i < files.size(); ++i) QFile::remove(files[i].absoluteFilePath());
    return true;
}
