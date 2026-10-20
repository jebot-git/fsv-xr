// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QString>

namespace Archives {
bool supported(const QString &path);
QString extractionName(const QString &path);
// Return an error message, or an empty string on success. Never overwrite.
QString extract(const QString &path);
QString create(const QString &source, const QString &destination);
}
