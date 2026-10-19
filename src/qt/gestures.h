// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QObject>
#include <QVariantList>
#include <QVector3D>
#include <QElapsedTimer>

// Gesture state is independent of the rendering API, allowing replay in tests.
// Units match Qt Quick 3D XR (centimetres); OpenXR joint indices are preserved.
class HandGesture : public QObject {
    Q_OBJECT
    Q_PROPERTY(int button READ button NOTIFY changed)
    Q_PROPERTY(bool dragging READ dragging NOTIFY changed)
public:
    explicit HandGesture(QObject *parent = nullptr);
    int button() const { return m_button; } // 1=index/LMB, 2=middle/MMB, 3=ring/RMB
    bool dragging() const { return m_button == 2; }
    Q_INVOKABLE void update(const QVariantList &joints, bool tracked);
    Q_INVOKABLE void reset();
    // Explicit monotonic timestamps make hysteresis and tracking-loss tests deterministic.
    void sample(const QList<QVector3D> &joints, bool tracked, qint64 timestamp);
signals:
    void changed();
private:
    int m_button = 0, m_candidate = 0;
    bool m_armed = false;
    qint64 m_since = 0, m_last = -1;
    QVector3D m_wrist;
    QElapsedTimer m_clock;
};
