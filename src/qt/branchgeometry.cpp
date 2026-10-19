// SPDX-License-Identifier: LGPL-2.1-or-later
#include "branchgeometry.h"
#include <QVector3D>

void BranchGeometry::setSegments(const QVariantList &segments)
{
    if (m_segments == segments) return;
    m_segments = segments;
    clear();
    QByteArray vertices;
    vertices.reserve(segments.size() * 36 * 3 * sizeof(float));
    static constexpr int triangles[] = {0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 3,7,6, 3,6,2, 0,4,7, 0,7,3, 1,2,6, 1,6,5};
    QVector3D minimum, maximum;
    bool first = true;
    for (const auto &value : segments) {
        const auto segment = value.toMap();
        const auto center = segment["center"].value<QVector3D>(), extent = segment["extent"].value<QVector3D>();
        if (extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) continue;
        const auto low = center - extent / 2, high = center + extent / 2;
        const QVector3D corners[] = {{low.x(),low.y(),low.z()}, {high.x(),low.y(),low.z()}, {high.x(),high.y(),low.z()}, {low.x(),high.y(),low.z()},
            {low.x(),low.y(),high.z()}, {high.x(),low.y(),high.z()}, {high.x(),high.y(),high.z()}, {low.x(),high.y(),high.z()}};
        if (first) { minimum = low; maximum = high; first = false; }
        else {
            minimum = {qMin(minimum.x(),low.x()), qMin(minimum.y(),low.y()), qMin(minimum.z(),low.z())};
            maximum = {qMax(maximum.x(),high.x()), qMax(maximum.y(),high.y()), qMax(maximum.z(),high.z())};
        }
        for (int index : triangles) {
            const float point[] = {corners[index].x(), corners[index].y(), corners[index].z()};
            vertices.append(reinterpret_cast<const char *>(point), sizeof(point));
        }
    }
    setVertexData(vertices);
    setStride(3 * sizeof(float));
    setPrimitiveType(PrimitiveType::Triangles);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    setBounds(minimum, maximum);
    update();
    emit segmentsChanged();
}
