// SPDX-License-Identifier: LGPL-2.1-or-later
#include "domed.h"
#include "filesystem.h"
#include "thumbnail.h"
#include "appearance.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QPainter>
#include <QResizeEvent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QDialog>
#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QThreadPool>
#include <QMenu>

namespace {
class PreviewPanel : public QDialog {
public:
    explicit PreviewPanel(QWidget *parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {}
protected:
    void paintEvent(QPaintEvent *) override {
        // WA_TranslucentBackground disables QWidget's automatic background fill.
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), palette().window());
    }
};
}

DomeWindow::DomeWindow(FileSystem *files, Appearance *appearance) : m_files(files), m_appearance(appearance)
{
    setWindowTitle("fsv-xr — DomeD");
    connect(files, &FileSystem::rootChanged, this, [this, files] { setWindowTitle("fsv-xr — " + files->root()); });
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1280, 820);
    m_scene.setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_scene.setClearColor(Qt::transparent);
    m_scene.resize(size());
    m_scene.engine()->addImageProvider("thumbnail", new ThumbnailProvider);
    m_scene.rootContext()->setContextProperty("files", files);
    m_scene.rootContext()->setContextProperty("appearance", appearance);
    m_scene.setAttribute(Qt::WA_DontShowOnScreen);
    m_scene.setSource(QUrl("qrc:/qml/Browser.qml"));
    if (m_scene.rootObject()) {
        m_scene.rootObject()->setProperty("spatialPopups", true);
        connect(m_scene.rootObject(), SIGNAL(previewRequested(QPointF)), this, SLOT(showPreview(QPointF)));
        connect(m_scene.rootObject(), SIGNAL(contextMenuRequested(QPointF)), this, SLOT(showContextMenu(QPointF)));
    }
    connect(files, &FileSystem::selectedChanged, this, [this] {
        // A reset, refresh, or another selection invalidates the popup's actions.
        if (m_preview) m_preview->close();
        if (m_menu) m_menu->close();
    });
    m_scene.show();
    // grabFramebuffer renders hidden QQuickWidgets with Qt's offscreen Vulkan RHI.
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!isVisible() || isMinimized()) return;
        m_frame = m_scene.grabFramebuffer();
        if (m_frame.isNull()) { qCritical("DomeD offscreen Vulkan rendering failed."); QCoreApplication::exit(1); return; }
        update();
    });
    m_timer.start(33);
}
DomeWindow::~DomeWindow()
{
    m_timer.stop();
    // Preview jobs own no widgets, but must finish before QApplication and its
    // font/image infrastructure are destroyed during process shutdown.
    QThreadPool::globalInstance()->waitForDone();
}
void DomeWindow::showPreview(const QPointF &anchor)
{
    const auto selection = m_files->selection();
    if (selection.isEmpty() || m_files->busy()) return;
    if (m_preview) delete m_preview.data();
    // Qt::Popup creates a real xdg_popup with an attachment point on the owning
    // raster Wayland surface. DomeD supplies placement, beams and hand routing.
    auto *panel = new PreviewPanel(this);
    m_preview = panel;
    panel->setObjectName("spatialFilePreview");
    panel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(panel, &QWidget::customContextMenuRequested, panel, [this, panel](const QPoint &point) { showMenu(panel, point); });
    panel->setAttribute(Qt::WA_TranslucentBackground);
    panel->setAutoFillBackground(true);
    auto updatePalette = [this, panel] {
        QPalette palette = QApplication::palette();
        palette.setColor(QPalette::Window, m_appearance->panelColor());
        panel->setPalette(palette);
        panel->update();
    };
    connect(m_appearance, &Appearance::changed, panel, updatePalette);
    updatePalette();
    panel->setWindowTitle("fsv-xr — " + selection.value("name").toString());
    panel->setFixedWidth(340);
    auto *layout = new QVBoxLayout(panel);
    auto label = [layout](const QString &text) {
        auto *item = new QLabel(text);
        item->setTextFormat(Qt::PlainText);
        item->setWordWrap(true);
        layout->addWidget(item);
        return item;
    };
    auto *title = label(selection.value("name").toString());
    title->setMaximumHeight(52);
    title->setToolTip(selection.value("name").toString());
    QFont font = title->font(); font.setBold(true); title->setFont(font);
    auto *image = label("Loading preview…");
    image->setFixedSize(316, 140);
    image->setAlignment(Qt::AlignCenter);
    label(selection.value("mime").toString() + "\n" + (m_files->inTrash() && selection.value("directory").toBool() ? QString("Folder") : QLocale().toString(selection.value("bytes").toLongLong()) + " bytes") + "\nModified: "
          + selection.value("modified").toString() + "\nOwner: " + selection.value("owner").toString()
          + (selection.value("symlink").toBool() ? "\nSymbolic link" : ""));
    if (selection.value("trashed").toBool())
        label("Original location: " + selection.value("originalPath").toString() + "\nDeleted: " + selection.value("deleted").toString());
    auto *path = label(selection.value("path").toString());
    path->setMaximumHeight(60);
    path->setToolTip(selection.value("path").toString());
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *open = buttons->addButton(m_files->inTrash() ? "Restore" : "Open", QDialogButtonBox::AcceptRole);
    open->setEnabled(!m_files->inTrash() || selection.value("canRestore").toBool());
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, panel, &QDialog::reject);
    connect(open, &QPushButton::clicked, panel, [this, panel, selection] {
        panel->close();
        if (m_files->selection().value("path") == selection.value("path")) m_files->open(m_files->selected());
    });
    const QString id = selection.value("thumbnail").toString().mid(QString("image://thumbnail/").size());
    auto *watcher = new QFutureWatcher<QImage>(panel);
    connect(watcher, &QFutureWatcher<QImage>::finished, panel, [watcher, image] {
        image->setText({});
        image->setPixmap(QPixmap::fromImage(watcher->result()).scaled(image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([id] {
        ThumbnailProvider provider;
        return provider.requestImage(id, nullptr, QSize(512, 512));
    }));
    panel->adjustSize();
    panel->move(mapToGlobal(anchor.toPoint()));
    panel->show();
}
void DomeWindow::showContextMenu(const QPointF &anchor)
{
    if (m_preview) m_preview->close();
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
            if (m_preview) m_preview->close();
            auto *actions = m_scene.rootObject()->findChild<QObject *>("fileActions");
            if (actions) QMetaObject::invokeMethod(actions, "edit", Q_ARG(QVariant, operation));
        });
    };
    if (m_files->inTrash()) {
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
    item("Open in New Tab", selected && m_files->selection().value("directory").toBool(), [this] { m_files->newTab(m_files->selection().value("path").toString()); });
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
    item("Delete Permanently…", selected, [this] {
        QTimer::singleShot(0, this, [this] {
            if (m_preview) m_preview->close();
            auto *actions = m_scene.rootObject()->findChild<QObject *>("fileActions");
            if (actions) QMetaObject::invokeMethod(actions, "confirmRemoval");
        });
    });
    menu->addSeparator();
    if (!m_files->inTrash()) {
    item("Create Folder…", true, [edit] { edit("mkdir"); });
    item("Open Terminal Here", true, [this] { m_files->terminal(); });
    }
    item("Properties", selected, [this] {
        QTimer::singleShot(0, this, [this] {
            QMetaObject::invokeMethod(m_scene.rootObject(), "previewSelection");
        });
    });
    // Export the full menu buffer. Let DomeD crop/scroll it, instead of stacking
    // QMenu's screen-height scrolling inside the compositor's own viewport.
    menu->ensurePolished();
    menu->setFixedSize(menu->sizeHint());
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
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: case QEvent::Wheel: case QEvent::KeyPress: case QEvent::KeyRelease:
    case QEvent::FocusIn: case QEvent::FocusOut: case QEvent::Enter: case QEvent::Leave:
    case QEvent::WindowActivate: case QEvent::WindowDeactivate:
        QCoreApplication::sendEvent(&m_scene, event);
        return event->isAccepted();
    default: return QWidget::event(event);
    }
}
