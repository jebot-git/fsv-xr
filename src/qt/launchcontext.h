// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QString>

struct LaunchContext {
    bool dome = false;
    bool spatial = false;
    static LaunchContext detect();
    static bool spatialForSocket(const QString &socketPath, qint64 compositorPid);
};
