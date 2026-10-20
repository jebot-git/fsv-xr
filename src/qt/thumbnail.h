// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QQuickImageProvider>
class ThumbnailProvider : public QQuickImageProvider {
public:
    ThumbnailProvider() : QQuickImageProvider(Image, ForceAsynchronousImageLoading) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    static QString url(const QString &path, qint64 modifiedMs = -1, bool archivePreview = false);
};
