// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include <QFileInfo>
#include <QRectF>
#include <QSizeF>
#include <cmath>
#include <limits>

void FileSystem::arrangeTree()
{
    // Directory platforms carry only their immediate files. Expanded child
    // directories occupy separate platforms, linked like the original TreeV.
    const int count = m_entries.size() + 1;
    QList<QList<int>> directories(count), leaves(count);
    QList<int> platforms{-1}, columns(count, 1);
    QList<QSizeF> sizes(count);
    QList<float> widths(count);
    int levels = 1;
    for (int i = 0; i < m_entries.size(); ++i) {
        const auto &entry = m_entries[i];
        if (entry.directory && !entry.symlink) {
            platforms << i;
            directories[entry.parent + 1] << i;
            levels = qMax(levels, entry.depth + 2);
        } else leaves[entry.parent + 1] << i;
    }
    QList<float> depths(levels, 0), levelZ(levels, 0);
    auto level = [&](int parent) { return parent < 0 ? 0 : m_entries[parent].depth + 1; };
    for (int parent : platforms) {
        const int n = leaves[parent + 1].size();
        columns[parent + 1] = qMax(1, qCeil(std::sqrt(double(n))));
        const int rows = (n + columns[parent + 1] - 1) / columns[parent + 1];
        sizes[parent + 1] = QSizeF(qMax(24, columns[parent + 1] * 16 + 8), qMax(32., (rows * 16 + 8) / .68));
        depths[level(parent)] = qMax(depths[level(parent)], float(sizes[parent + 1].height()));
    }
    for (auto it = platforms.crbegin(); it != platforms.crend(); ++it) {
        const int parent = *it;
        float childWidth = 0;
        for (int child : directories[parent + 1]) childWidth += widths[child + 1] + 20;
        if (childWidth > 0) childWidth -= 20;
        widths[parent + 1] = qMax(float(sizes[parent + 1].width()), childWidth);
    }
    for (int i = 1; i < levels; ++i) levelZ[i] = levelZ[i - 1] - depths[i - 1] / 2 - 24 - depths[i] / 2;
    QVector3D rootCenter, rootExtent;
    QRectF bounds;
    std::function<void(int, float)> place = [&](int parent, float left) {
        const auto size = sizes[parent + 1];
        const float x = left + widths[parent + 1] / 2 - widths[0] / 2, z = levelZ[level(parent)];
        const QVector3D center(x, .9f, z), extent(size.width(), 1.8f, size.height());
        if (parent < 0) { rootCenter = center; rootExtent = extent; }
        else { m_entries[parent].center = center; m_entries[parent].extent = extent; }
        const QRectF rect(x - size.width() / 2, z - size.height() / 2, size.width(), size.height());
        bounds = bounds.isNull() ? rect : bounds.united(rect);
        int j = 0;
        for (int child : leaves[parent + 1]) {
            auto &entry = m_entries[child];
            const float height = 2 + qMin(15.f, float(std::log2(1. + double(entry.bytes))) * .55f);
            entry.extent = {12, height, 12};
            entry.center = {float(rect.left() + 12 + (j % columns[parent + 1]) * 16), 1.8f + height / 2,
                float(rect.top() + 12 + (j / columns[parent + 1]) * 16)};
            ++j;
        }
        float childWidth = 0;
        for (int child : directories[parent + 1]) childWidth += widths[child + 1] + 20;
        if (childWidth > 0) childWidth -= 20;
        float cursor = left + (widths[parent + 1] - childWidth) / 2;
        for (int child : directories[parent + 1]) { place(child, cursor); cursor += widths[child + 1] + 20; }
    };
    place(-1, 0);
    struct Segment { QVector3D from, to; int parent, child; };
    QList<Segment> segments;
    auto line = [&](const QVector3D &from, const QVector3D &to, int parent, int child = -2) {
        if ((to - from).length() > .001f) segments.append({from, to, parent, child});
    };
    for (int parent : platforms) {
        const auto &kids = directories[parent + 1];
        if (kids.isEmpty()) continue;
        const auto center = parent < 0 ? rootCenter : m_entries[parent].center;
        const auto extent = parent < 0 ? rootExtent : m_entries[parent].extent;
        const float back = center.z() - extent.z() / 2;
        float childFront = -std::numeric_limits<float>::max(), minX = center.x(), maxX = center.x();
        for (int child : kids) {
            const auto &entry = m_entries[child];
            childFront = qMax(childFront, entry.center.z() + entry.extent.z() / 2);
            minX = qMin(minX, entry.center.x()); maxX = qMax(maxX, entry.center.x());
        }
        const float junction = (back + childFront) / 2;
        line({center.x(), .9f, back}, {center.x(), .9f, junction}, parent);
        line({minX, .9f, junction}, {maxX, .9f, junction}, parent);
        for (int child : kids) {
            const auto &entry = m_entries[child];
            line({entry.center.x(), .9f, junction}, {entry.center.x(), .9f, entry.center.z() + entry.extent.z() / 2}, parent, child);
        }
    }
    // Uniformly fit the overview, preserving square file footprints. Users can
    // zoom onto a platform without the horizontal compression of a nested grid.
    const float factor = qMin(2.5, qMin(160. / bounds.width(), 200. / bounds.height()));
    auto transform = [&](QVector3D point) { return QVector3D(point.x() - bounds.center().x(), point.y(), point.z() - bounds.center().y()) * factor; };
    for (auto &entry : m_entries) { entry.center = transform(entry.center); entry.extent *= factor; }
    m_treeRoot = {{"path", m_root}, {"name", inTrash() ? tr("Trash") : m_root == "/" ? QString("/") : QFileInfo(m_root).fileName()},
        {"center", QVariant::fromValue(transform(rootCenter))}, {"extent", QVariant::fromValue(rootExtent * factor)}};
    for (const auto &segment : segments) {
        const auto from = transform(segment.from), to = transform(segment.to);
        const QVector3D extent(qMax(1.2f * factor, qAbs(to.x() - from.x())), .65f * factor, qMax(1.2f * factor, qAbs(to.z() - from.z())));
        m_treeBranches.append(QVariantMap{{"center", QVariant::fromValue((from + to) / 2)}, {"extent", QVariant::fromValue(extent)},
            {"from", QVariant::fromValue(from)}, {"to", QVariant::fromValue(to)}, {"parent", segment.parent}, {"child", segment.child}});
    }
}
