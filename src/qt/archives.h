// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QString>
#include <atomic>
#include <memory>

struct ScanResult;

namespace Archives {
bool supported(const QString &path);
QString extractionName(const QString &path);
// Return an error message, or an empty string on success. Never overwrite.
QString extract(const QString &path);
QString create(const QString &source, const QString &destination);

// A location keeps the archive filename and member path in separate URL fields.
bool isLocation(const QString &path);
QString location(const QString &file, const QString &member = {});
QString source(const QString &path);
QString member(const QString &path);
bool validLocation(const QString &path);
QString parent(const QString &path);
QString title(const QString &path);
ScanResult browse(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel, bool reload = false);
struct Materialized { QString path, error; };
// Private copies live until application exit; bounded to 64 MiB/file, 256 MiB total.
Materialized materialize(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel = {});
}
