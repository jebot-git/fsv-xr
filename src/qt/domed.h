// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QWidget>
#include <QQuickWidget>
#include <QTimer>
#include <QImage>
#include <QPointer>

class QDialog;
class QMenu;

class FileSystem;
class Appearance;
// DomeD currently accepts wl_shm client buffers. Render the same scene offscreen
// with Vulkan, read back at 30 Hz, then present a raster Wayland surface. No GL
// context, competing OpenXR session, or compositor protocol extension is needed.
class DomeWindow : public QWidget {
    Q_OBJECT
public:
    explicit DomeWindow(FileSystem *files, Appearance *appearance, bool spatial = true);
    ~DomeWindow() override;
    bool ready() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *event) override;
private slots:
    void showPreview(const QPointF &anchor);
    void showContextMenu(const QPointF &anchor);
private:
    void showMenu(QWidget *owner, const QPointF &anchor);
    bool m_haveInput = false;
    FileSystem *m_files;
    Appearance *m_appearance;
    QPointer<QDialog> m_preview;
    QPointer<QMenu> m_menu;
    QQuickWidget m_scene;
    QTimer m_timer;
    QImage m_frame;
};
