// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QObject>
#include <QColor>
#include <QFileSystemWatcher>
#include <QPalette>

class Appearance : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor sceneBackground READ sceneBackground NOTIFY changed)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY changed)
public:
    explicit Appearance(bool dome, QObject *parent = nullptr);
    QColor sceneBackground() const { return m_scene; }
    QColor panelColor() const { return m_panel; }
signals:
    void changed();
private:
    void reload();
    void watch();
    QString m_path;
    QColor m_scene, m_panel;
    QPalette m_original;
    QFileSystemWatcher m_watcher;
};
