// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "filesystem.h"

struct TrashLocation {
    QString directory, topDirectory;
    bool home = false;
};
class TrashStore {
public:
    static QList<TrashLocation> locations();
    static QStringList watchPaths();
    static ScanResult scan(const QList<TrashLocation> &locations,
                           const std::shared_ptr<std::atomic_bool> &cancel, int limit = 2500);
    static QString restore(const Entry &entry);
    static QString erase(const Entry &entry);
    static QString empty(const QList<TrashLocation> &locations);
};
