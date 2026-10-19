// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gestures.h"
#include <cmath>
HandGesture::HandGesture(QObject *parent) : QObject(parent) { m_clock.start(); }
void HandGesture::reset()
{
    m_armed = false; m_candidate = 0; m_last = -1;
    if (m_button) { m_button = 0; emit changed(); }
}
void HandGesture::update(const QVariantList &joints, bool tracked)
{
    QList<QVector3D> points;
    for (const auto &joint : joints) points << joint.value<QVector3D>();
    sample(points, tracked, m_clock.elapsed());
}
void HandGesture::sample(const QList<QVector3D> &joints, bool tracked, qint64 now)
{
    if (!tracked || joints.size() != 26) { reset(); return; }
    for (const auto &p : joints)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z())) { reset(); return; }
    // Palm width normalizes across hand sizes and world scale.
    const float width = (joints[6] - joints[21]).length();
    if (width < 1.0f || width > 16.0f) { reset(); return; }
    if (m_last >= 0 && (now - m_last > 200 || (joints[1] - m_wrist).length() > 25)) {
        reset(); m_last = now; m_wrist = joints[1]; return;
    }
    m_last = now; m_wrist = joints[1];
    float ratios[3];
    for (int i = 0; i < 3; ++i) ratios[i] = (joints[5] - joints[10 + i * 5]).length() / width;
    if (!m_armed) {
        if (ratios[0] > .65f && ratios[1] > .65f && ratios[2] > .65f) m_armed = true;
        return;
    }
    if (m_button) {
        if (ratios[m_button - 1] > .60f) { m_button = 0; m_candidate = 0; emit changed(); }
        return;
    }
    int candidate = 0;
    for (int i = 0; i < 3; ++i) {
        if (ratios[i] < .38f) {
            if (candidate) { m_candidate = 0; return; } // Ambiguous/fist: no click.
            candidate = i + 1;
        }
    }
    if (candidate != m_candidate) { m_candidate = candidate; m_since = now; }
    if (candidate && now - m_since >= 55) { m_button = candidate; emit changed(); }
}
