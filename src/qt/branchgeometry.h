// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QQuick3DGeometry>
#include <QVariant>

class BranchGeometry : public QQuick3DGeometry {
    Q_OBJECT
    Q_PROPERTY(QVariantList segments READ segments WRITE setSegments NOTIFY segmentsChanged)
public:
    explicit BranchGeometry(QQuick3DObject *parent = nullptr) : QQuick3DGeometry(parent) {}
    QVariantList segments() const { return m_segments; }
    void setSegments(const QVariantList &segments);
signals:
    void segmentsChanged();
private:
    QVariantList m_segments;
};
