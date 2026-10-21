// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QObject>
#include <QColor>
#include <QFileSystemWatcher>
#include <QPalette>
#include "domeappearance.h"

class Appearance : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dome READ dome CONSTANT)
    Q_PROPERTY(QFont font READ font NOTIFY changed)
    Q_PROPERTY(QColor sceneBackground READ sceneBackground NOTIFY changed)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY changed)
public:
    explicit Appearance(bool dome, QObject *parent = nullptr);
    bool dome() const { return m_fonts.enabled(); }
    QFont font() const { return m_fonts.font(); }
    QColor sceneBackground() const { return m_scene; }
    QColor panelColor() const { return m_panel; }
signals:
    void changed();
private:
    void reload();
    void watch();
    DomeAppearance m_fonts;
    QString m_path;
    QColor m_scene, m_panel;
    QPalette m_original;
    QFileSystemWatcher m_watcher;
};
