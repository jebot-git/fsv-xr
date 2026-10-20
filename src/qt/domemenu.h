// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QMenuBar>
#include <QHash>
#include <functional>

class FileSystem;
// Native Qt menus are exported over DBusMenu and associated with this Wayland
// window by Qt. DomeD owns global placement, navigation and realm controls.
class DomeMenu : public QMenuBar {
    Q_OBJECT
public:
    DomeMenu(FileSystem *files, QWidget *owner);
    void setTextEditing(bool editing);
signals:
    void commandRequested(const QString &command);
private:
    void updateActions();
    QAction *item(QMenu *menu, const QString &id, const QString &label,
                  const QString &shortcut, const std::function<void()> &callback);
    bool m_textEditing = false;
    FileSystem *m_files;
    QHash<QString, QAction *> m_actions;
};
