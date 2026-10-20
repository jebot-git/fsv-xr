// SPDX-License-Identifier: LGPL-2.1-or-later
#include "domed.h"
#include "domemenu.h"
#include "filesystem.h"
#include "thumbnail.h"
#include "appearance.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QPainter>
#include <QResizeEvent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QApplication>
#include <QMenu>

DomeWindow::DomeWindow(FileSystem *files, Appearance *appearance, bool spatial) : m_files(files)
{
    setWindowTitle("fsvr — DomeD");
    connect(files, &FileSystem::rootChanged, this, [this, files] { setWindowTitle("fsvr — " + files->root()); });
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1280, 820);
    // Keep the raster surface as the real translucent toplevel. Qt publishes
    // this menu through the registrar and associates it with that surface.
    auto *menus = new DomeMenu(files, this);
    menus->setVisible(menus->isNativeMenuBar());
    connect(menus, &DomeMenu::commandRequested, this, [this](const QString &command) {
        if (m_scene.rootObject()) QMetaObject::invokeMethod(m_scene.rootObject(), "menuCommand", Q_ARG(QVariant, command));
    });
    // The offscreen renderer must not keep a closed file-manager process alive.
    m_scene.setAttribute(Qt::WA_QuitOnClose, false);
    m_scene.setMouseTracking(true);
    m_scene.setFocusPolicy(Qt::StrongFocus);
    m_scene.setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_scene.setClearColor(Qt::transparent);
    m_scene.resize(size());
    m_scene.engine()->addImageProvider("thumbnail", new ThumbnailProvider);
    m_scene.rootContext()->setContextProperty("files", files);
    m_scene.rootContext()->setContextProperty("appearance", appearance);
    m_scene.setAttribute(Qt::WA_DontShowOnScreen);
    m_scene.setSource(QUrl("qrc:/qml/Browser.qml"));
    if (m_scene.rootObject()) {
        m_scene.rootObject()->setProperty("spatialPopups", spatial);
        m_scene.rootObject()->setProperty("nativeMenus", true);
        connect(m_scene.rootObject(), SIGNAL(contextMenuRequested(QPointF)), this, SLOT(showContextMenu(QPointF)));
    }
    connect(m_scene.quickWindow(), &QQuickWindow::activeFocusItemChanged, menus, [this, menus] {
        auto *focus = m_scene.quickWindow()->activeFocusItem();
        menus->setTextEditing(focus && focus->metaObject()->indexOfMethod("copy()") >= 0);
    });
    connect(files, &FileSystem::selectedChanged, this, [this] {
        // A reset, refresh, or another selection invalidates menu actions.
        if (m_menu) m_menu->close();
    });
    // QQuickWidget renders on the GUI thread. Track actual scene renders so
    // asynchronous thumbnails, animations and theme changes all invalidate the
    // raster copy without reading back an unchanged framebuffer while idle.
    connect(m_scene.quickWindow(), &QQuickWindow::afterRendering, this, [this] {
        if (!m_capturing) m_frameDirty = true;
    }, Qt::DirectConnection);
    m_scene.show();
    // grabFramebuffer renders hidden QQuickWidgets with Qt's offscreen Vulkan RHI.
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_frameDirty || !isVisible() || isMinimized()) return;
        m_frameDirty = false;
        // grabFramebuffer itself renders; that render must not schedule another
        // capture indefinitely. Subsequent scene updates mark the frame dirty.
        m_capturing = true;
        m_frame = m_scene.grabFramebuffer();
        m_capturing = false;
        if (m_frame.isNull()) { qCritical("DomeD offscreen Vulkan rendering failed."); QCoreApplication::exit(1); return; }
        update();
    });
    m_timer.start(33);
}
DomeWindow::~DomeWindow()
{
    m_timer.stop();
}
void DomeWindow::showContextMenu(const QPointF &anchor)
{
    showMenu(this, anchor);
}
void DomeWindow::showMenu(QWidget *owner, const QPointF &anchor)
{
    if (m_files->busy()) return;
    if (m_menu) { m_menu->close(); m_menu->deleteLater(); }
    auto *menu = new QMenu(owner);
    m_menu = menu;
    menu->setObjectName("spatialFileMenu");
    // Create an xdg_popup immediately from the real RMB event. DomeD associates
    // that event's surface/coordinates with this menu and owns placement,
    // viewport scroll controls and spatial hit testing. Qt owns menu keys.
    const bool selected = m_files->selected() >= 0;
    auto item = [menu](const QString &name, bool enabled, const std::function<void()> &action) {
        auto *entry = menu->addAction(name); entry->setEnabled(enabled);
        QObject::connect(entry, &QAction::triggered, menu, action);
    };
    auto edit = [this](const QString &operation) {
        QTimer::singleShot(0, this, [this, operation] {
            auto *actions = m_scene.rootObject()->findChild<QObject *>("fileActions");
            if (actions) QMetaObject::invokeMethod(actions, "edit", Q_ARG(QVariant, operation));
        });
    };
    if (m_files->inArchive()) {
        const bool canOpen = selected && m_files->selection().value("canOpen").toBool();
        item("Open", canOpen, [this] { m_files->open(m_files->selected()); });
        item("Open in New Tab", selected && (m_files->selection().value("directory").toBool() || m_files->selection().value("archive").toBool()), [this] { m_files->newTab(m_files->selection().value("path").toString()); });
        item("Open With…", canOpen && !m_files->selection().value("directory").toBool(), [edit] { edit("openWith"); });
        item("Copy Location", selected, [this] { m_files->copyLocation(); });
    } else if (m_files->inTrash()) {
        item("Restore", selected && m_files->selection().value("canRestore").toBool(), [this] { m_files->restore(); });
        item("Empty Trash…", true, [this] {
            QTimer::singleShot(0, this, [this] {
                auto *actions = m_scene.rootObject()->findChild<QObject *>("fileActions");
                if (actions) QMetaObject::invokeMethod(actions, "confirmEmptyTrash");
            });
        });
        item("Copy Location", selected, [this] { m_files->copyLocation(); });
        menu->addSeparator();
    } else {
    item("Open", selected, [this] { m_files->open(m_files->selected()); });
    item("Open in New Tab", selected && (m_files->selection().value("directory").toBool() || m_files->selection().value("archive").toBool()), [this] { m_files->newTab(m_files->selection().value("path").toString()); });
    if (m_files->selection().value("archive").toBool()) {
        item("Browse Archive", true, [this] { m_files->browseArchive(); });
        item("Extract Archive", !m_files->busy(), [this] { m_files->extractArchive(); });
    }
    item("Compress…", selected && !m_files->busy(), [edit] { edit("compress"); });
    item("Open With…", selected && !m_files->selection().value("directory").toBool(), [edit] { edit("openWith"); });
    menu->addSeparator();
    item("Cut", selected, [this] { m_files->clipboard(true); });
    item("Copy", selected, [this] { m_files->clipboard(false); });
    item("Paste", m_files->canPaste(), [this] { m_files->paste(); });
    item("Copy Location", selected, [this] { m_files->copyLocation(); });
    menu->addSeparator();
    item("Rename…", selected, [edit] { edit("rename"); });
    item("Move to Trash", selected, [this] { m_files->remove(false); });
    }
    if (!m_files->inArchive()) item("Delete Permanently…", selected, [this] {
        QTimer::singleShot(0, this, [this] {
            auto *actions = m_scene.rootObject()->findChild<QObject *>("fileActions");
            if (actions) QMetaObject::invokeMethod(actions, "confirmRemoval");
        });
    });
    menu->addSeparator();
    if (!m_files->inTrash() && !m_files->inArchive()) {
    item("Create Folder…", true, [edit] { edit("mkdir"); });
    item("Open Terminal Here", true, [this] { m_files->terminal(); });
    }
    item("Properties", selected, [this] {
        QTimer::singleShot(0, this, [this] {
            QMetaObject::invokeMethod(m_scene.rootObject(), "showInspector");
        });
    });
    // Export the full menu buffer. Let DomeD crop/scroll it, instead of stacking
    // QMenu's screen-height scrolling inside the compositor's own viewport.
    menu->ensurePolished();
    menu->setFixedSize(menu->sizeHint());
    // The hidden Quick widget does not receive the native owner's mouse grab
    // loss when QMenu opens. Cancel its current press so the next click starts
    // a fresh gesture after the native popup closes.
    QTimer::singleShot(0, this, [this] {
        if (auto *grabber = m_scene.quickWindow()->mouseGrabberItem()) grabber->ungrabMouse();
    });
    menu->popup(owner->mapToGlobal(anchor.toPoint()));
}
bool DomeWindow::ready() const { return m_scene.status() == QQuickWidget::Ready; }
void DomeWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(rect(), m_frame);
}
void DomeWindow::resizeEvent(QResizeEvent *event) { m_scene.resize(event->size()); QWidget::resizeEvent(event); }
bool DomeWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::DragEnter: case QEvent::DragMove: case QEvent::DragLeave: case QEvent::Drop:
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseButtonDblClick:
    case QEvent::ShortcutOverride:
    case QEvent::MouseMove: case QEvent::Wheel: case QEvent::KeyPress: case QEvent::KeyRelease:
    case QEvent::FocusIn: case QEvent::FocusOut: case QEvent::Enter: case QEvent::Leave:
    case QEvent::WindowActivate: case QEvent::WindowDeactivate:
        QCoreApplication::sendEvent(&m_scene, event);
        return event->isAccepted();
    default: return QWidget::event(event);
    }
}
