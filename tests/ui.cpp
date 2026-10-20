// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "thumbnail.h"
#include "gestures.h"
#include "branchgeometry.h"
#include <QtTest>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QFile>
#include <QPainter>
#include <QStandardPaths>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>

class UiTest : public QObject {
    Q_OBJECT
    static QQuickItem *visualItem(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems()) if (auto *found = visualItem(child, name)) return found;
        return nullptr;
    }
private slots:
    void nativeFileDrop() {
        QTemporaryDir source, destination;
        const QString filename = "drop # 漢字.txt";
        QFile file(source.filePath(filename)); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("native URI drop"); file.close();
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.navigate(destination.path()); QTRY_VERIFY(!files.busy());
        QVERIFY(QTest::qWaitForWindowExposed(window)); QTest::qWait(250);
        auto *area = window->findChild<QQuickItem *>("fileDropArea"); QVERIFY(area);
        const QPoint point = area->mapToScene(QPointF(area->width()/2, area->height()/2)).toPoint();
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(source.filePath(filename))});
        QDragEnterEvent enter(point, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::ControlModifier);
        QCoreApplication::sendEvent(window, &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(point, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::ControlModifier);
        QCoreApplication::sendEvent(window, &drop); QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
        QTRY_VERIFY(!files.busy());
        QVERIFY(QFileInfo::exists(destination.filePath(filename)));
        QVERIFY(QFileInfo::exists(source.filePath(filename)));
        // Move the same URI onto a directory tab, through its real DropArea.
        QTemporaryDir tabDestination;
        files.newTab(tabDestination.path()); QTRY_VERIFY(!files.busy());
        files.activateTab(0); QTRY_VERIFY(!files.busy()); QTest::qWait(100);
        auto *tab = visualItem(window->contentItem(), "directoryTab1"); QVERIFY(tab);
        const QPoint tabPoint = tab->mapToScene(QPointF(10, tab->height()/2)).toPoint();
        QDragEnterEvent tabEnter(tabPoint, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(window, &tabEnter); QVERIFY(tabEnter.isAccepted());
        QDropEvent tabDrop(tabPoint, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(window, &tabDrop); QVERIFY(tabDrop.isAccepted());
        QCOMPARE(tabDrop.dropAction(), Qt::MoveAction);
        QTRY_VERIFY(!files.busy());
        QVERIFY(QFileInfo::exists(tabDestination.filePath(filename)));
        QVERIFY(!QFileInfo::exists(source.filePath(filename)));

    }
    void internalTrashInteractions() {
        const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
        QVERIFY(QDir().mkpath(trash + "/files")); QVERIFY(QDir().mkpath(trash + "/info"));
        QTemporaryDir destination;
        for (const auto &name : {"restore.txt", "erase.txt"}) {
            QFile file(trash + "/files/" + name); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Trash UI fixture");
            QFile info(trash + "/info/" + name + ".trashinfo"); QVERIFY(info.open(QIODevice::WriteOnly));
            info.write("[Trash Info]\nPath=" + QUrl::toPercentEncoding(destination.filePath(name), "/") + "\nDeletionDate=2026-10-19T12:34:56\n");
        }
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.rootContext()->setContextProperty("xrMode", false);
        engine.rootContext()->setContextProperty("domeMode", false);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.openPlace("trash:/"); QTRY_VERIFY(!files.busy());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        auto select = [&](const QString &name) {
            for (int i = 0; i < files.rowCount(); ++i)
                if (files.data(files.index(i), FileSystem::Path).toString() == trash + "/files/" + name) { files.setSelected(i); return true; }
            return false;
        };
        QVERIFY(select("restore.txt"));
        auto restore = window->findChild<QQuickItem *>("inspectorOpen"); QVERIFY(restore);
        QTRY_COMPARE(restore->property("text").toString(), QString("Restore"));
        QVERIFY(restore->property("enabled").toBool());
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save("fsvr-trash-preview.png"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, restore->mapToScene(QPointF(restore->width()/2, restore->height()/2)).toPoint());
        QTRY_VERIFY(!files.busy()); QTRY_VERIFY(QFileInfo::exists(destination.filePath("restore.txt")));
        QVERIFY(!QFileInfo::exists(trash + "/info/restore.txt.trashinfo"));
        QVERIFY(select("erase.txt"));
        auto actions = window->findChild<QObject *>("fileActions"); QVERIFY(actions);
        QVERIFY(QMetaObject::invokeMethod(actions, "show", Q_ARG(QVariant, 230), Q_ARG(QVariant, 130)));
        auto menu = window->findChild<QObject *>("contextMenu"); QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        bool restorePresent = false, emptyPresent = false;
        for (auto item : menu->findChildren<QQuickItem *>()) {
            const auto text = item->property("text").toString();
            if (text == "Restore") { QVERIFY(item->isVisible()); restorePresent = true; }
            if (text == "Empty Trash…") { QVERIFY(item->isVisible()); emptyPresent = true; }
            if (text == "Rename…" || text == "Move to Trash" || text == "Paste") QVERIFY(!item->isVisible());
        }
        QVERIFY(restorePresent); QVERIFY(emptyPresent);
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QVERIFY(QMetaObject::invokeMethod(actions, "confirmEmptyTrash"));
        auto empty = window->findChild<QObject *>("emptyTrashDialog"); QVERIFY(empty);
        QTRY_VERIFY(empty->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(empty, "reject")); // Never empty any real mounted Trash during tests.
        QVERIFY(QFileInfo::exists(trash + "/files/erase.txt"));
        QVERIFY(QMetaObject::invokeMethod(actions, "confirmRemoval"));
        auto confirm = window->findChild<QObject *>("confirmDeleteDialog"); QVERIFY(confirm);
        QTRY_VERIFY(confirm->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirm, "reject"));
        QVERIFY(QFileInfo::exists(trash + "/files/erase.txt"));
        QVERIFY(QMetaObject::invokeMethod(actions, "confirmRemoval"));
        QVERIFY(QMetaObject::invokeMethod(confirm, "accept"));
        QTRY_VERIFY(!files.busy()); QTRY_VERIFY(!QFileInfo::exists(trash + "/files/erase.txt"));
        QVERIFY(!QFileInfo::exists(trash + "/info/erase.txt.trashinfo"));
        QVERIFY(!QFileInfo::exists(destination.filePath("erase.txt")));
        window->close();
    }
    void treeRenderingAndPicking() {
        QTemporaryDir temp;
        QDir root(temp.path());
        QVERIFY(root.mkpath("Documents/Notes")); QVERIFY(root.mkpath("Pictures")); QVERIFY(root.mkpath("Downloads"));
        for (const auto &name : {"README.txt", "notes.txt", "Documents/design.txt", "Documents/Notes/todo.txt", "Pictures/photo.txt", "Downloads/archive.txt"}) {
            QFile file(root.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Tree platform preview");
        }
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.rootContext()->setContextProperty("xrMode", false);
        engine.rootContext()->setContextProperty("domeMode", false);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy()); files.setLayout("tree");
        QVERIFY(QTest::qWaitForWindowExposed(window)); QTest::qWait(500);
        auto connections = window->findChild<QObject *>("treeConnections"); QVERIFY(connections);
        QVERIFY(connections->property("visible").toBool());
        auto world = window->findChild<QObject *>("filesystemWorld"); QVERIFY(world);
        auto view = window->findChild<QQuickItem *>("filesystemView"); QVERIFY(view);
        int selected = -1;
        for (int i = 0; i < files.rowCount(); ++i)
            if (files.data(files.index(i), FileSystem::Path).toString() == root.filePath("Documents/design.txt")) selected = i;
        QVERIFY(selected >= 0);
        const auto center = files.data(files.index(selected), FileSystem::Center).value<QVector3D>();
        const auto extent = files.data(files.index(selected), FileSystem::Extent).value<QVector3D>();
        QVector3D scene, point;
        QVERIFY(QMetaObject::invokeMethod(world, "mapPositionToScene", Q_RETURN_ARG(QVector3D, scene), Q_ARG(QVector3D, center + QVector3D(0, extent.y()/2 + .1, 0))));
        QVERIFY(QMetaObject::invokeMethod(view, "mapFrom3DScene", Q_RETURN_ARG(QVector3D, point), Q_ARG(QVector3D, scene)));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(QPointF(point.x(), point.y())).toPoint());
        QTRY_COMPARE(files.selected(), selected);
        const auto frame = window->grabWindow(); QVERIFY(!frame.isNull());
        QVERIFY(frame.save("fsvr-tree-preview.png"));
        int branchPixels = 0;
        for (int y = 100; y < frame.height() - 30; ++y) for (int x = 210; x < frame.width() - 320; ++x) {
            const auto color = frame.pixelColor(x, y);
            if (color.red() > 140 && color.green() < 120 && color.blue() > 35
                && color.red() > color.green()*1.7 && color.green() > color.blue()*1.05) ++branchPixels;
        }
        QVERIFY2(branchPixels > 20, "Tree connector mesh was not rendered");
    }
    void zoomKeepsSelectedItemAnchored_data() {
        QTest::addColumn<QString>("layout");
        QTest::newRow("map") << QString("map");
        QTest::newRow("tree") << QString("tree");
    }
    void zoomKeepsSelectedItemAnchored() {
        QFETCH(QString, layout);
        QTemporaryDir temp;
        for (const auto &name : {"left.txt", "right.txt"}) {
            QFile file(temp.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("anchor");
        }
        if (layout == "tree") for (int i = 0; i < 8; ++i) QVERIFY(QDir(temp.path()).mkdir(QString("Directory%1").arg(i)));
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.rootContext()->setContextProperty("xrMode", false);
        engine.rootContext()->setContextProperty("domeMode", false);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy()); files.setLayout(layout);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        window->setProperty("yaw", 31.); window->setProperty("pitch", 18.);
        window->setProperty("panX", 7.); window->setProperty("panY", 3.); window->setProperty("panZ", -5.);
        window->setProperty("zoom", 1.6);
        files.setSelected(0);
        const auto center = files.selection()["center"].value<QVector3D>();
        const auto size = files.selection()["extent"].value<QVector3D>();
        const QVector3D anchor = center + QVector3D(0, size.y()/2 + qMin(.3f, size.y()/6), 0);
        auto world = window->findChild<QObject *>("filesystemWorld"); QVERIFY(world);
        auto view = window->findChild<QQuickItem *>("filesystemView"); QVERIFY(view);
        auto position = [&] {
            QVector3D result;
            if (!QMetaObject::invokeMethod(world, "mapPositionToScene", Q_RETURN_ARG(QVector3D, result), Q_ARG(QVector3D, anchor)))
                QTest::qFail("Cannot map selected anchor", __FILE__, __LINE__);
            return result;
        };
        const QPoint point = view->mapToScene(QPointF(view->width()*.9, view->height()*.2)).toPoint();
        auto wheel = [&](int delta) {
            QWheelEvent event(point, window->mapToGlobal(point), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &event);
        };
        QTest::qWait(50);
        const auto before = position();
        wheel(1200); QTest::qWait(50);
        QVERIFY(window->property("zoom").toReal() > 1.6);
        QVERIFY((position() - before).length() < .001);
        wheel(-1200); QTest::qWait(50);
        QVERIFY(qAbs(window->property("zoom").toReal() - 1.6) < .0001);
        QVERIFY((position() - before).length() < .001);
        auto navigation = window->findChild<QQuickItem *>("navigationMouse"); QVERIFY(navigation);
        auto middleClick = [&] {
            // Keep cursor warps from a previous native window out of this
            // gesture. Deliver the same seat press/release directly.
            QMouseEvent press(QEvent::MouseButtonPress, point, window->mapToGlobal(point), Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, window->mapToGlobal(point), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press); QCoreApplication::sendEvent(window, &release);
        };
        middleClick();
        QVERIFY(navigation->property("scrollMode").toBool());
        QMouseEvent move(QEvent::MouseMove, point - QPoint(0, 30), window->mapToGlobal(point - QPoint(0, 30)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        QVERIFY(window->property("zoom").toReal() > 1.6);
        middleClick();
        QTest::qWait(50); QVERIFY((position() - before).length() < .001);
        for (int delta : {120000, 120000, -120000, -120000}) {
            wheel(delta); QTest::qWait(20);
            QVERIFY((position() - before).length() < .001);
        }
        QCOMPARE(window->property("zoom").toReal(), .15);
        files.setSelected(-1);
        const auto state = files.viewState(); wheel(120);
        QCOMPARE(window->property("panX").toReal(), state["panX"].toReal());
        QCOMPARE(window->property("panY").toReal(), state["panY"].toReal());
        QCOMPARE(window->property("panZ").toReal(), state["panZ"].toReal());
    }
    void tabsAndCursorKeys() {
        QTemporaryDir temp;
        QFile file(temp.filePath("test.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("test"); file.close();
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.rootContext()->setContextProperty("xrMode", false);
        engine.rootContext()->setContextProperty("domeMode", false);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto navigation = window->findChild<QQuickItem *>("navigationMouse"); QVERIFY(navigation);
        navigation->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Right); QCOMPARE(window->property("yaw").toReal(), 5.);
        QTest::keyClick(window, Qt::Key_Left); QCOMPARE(window->property("yaw").toReal(), 0.);
        QTest::keyClick(window, Qt::Key_Down); QCOMPARE(window->property("pitch").toReal(), 5.);
        QTest::keyClick(window, Qt::Key_Up); QCOMPARE(window->property("pitch").toReal(), 0.);
        for (int i = 0; i < 20; ++i) QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(window->property("pitch").toReal(), 45.);
        auto location = window->findChild<QQuickItem *>("locationField"); QVERIFY(location);
        location->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Left); QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(window->property("yaw").toReal(), 0.); QCOMPARE(window->property("pitch").toReal(), 45.);
        QTest::keyClick(window, Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(files.tabs().size(), 2); QCOMPARE(files.currentTab(), 1);
        QCOMPARE(window->property("pitch").toReal(), 0.);
        navigation->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(window->property("yaw").toReal(), 5.);
        QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier);
        QTRY_COMPARE(files.currentTab(), 0);
        QCOMPARE(window->property("pitch").toReal(), 45.);
        QCOMPARE(window->property("yaw").toReal(), 0.);
        QQuickItem *close = nullptr;
        QTRY_VERIFY((close = visualItem(window->contentItem(), "closeTab1")));
        const auto closePoint = close->mapToScene(QPointF(close->width()/2, close->height()/2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, closePoint);
        QTRY_COMPARE(files.tabs().size(), 1);
        auto blank = window->findChild<QQuickItem *>("emptyTabBar"); QVERIFY(blank);
        QVERIFY(blank->width() > 0);
        const auto blankPoint = blank->mapToScene(QPointF(blank->width()/2, blank->height()/2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, blankPoint);
        QCOMPARE(files.tabs().size(), 1);
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, blankPoint);
        QTRY_COMPARE(files.tabs().size(), 2);
        QCOMPARE(files.currentTab(), 1); QCOMPARE(files.root(), temp.path());
    }
    void desktopRenderingPickingAndMenus() {
        QTemporaryDir temp, artifacts, placesData, destination;
        const QByteArray previousData = qgetenv("XDG_DATA_HOME");
        struct RestoreData { QByteArray value; ~RestoreData() { if (value.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", value); } } restoreData{previousData};
        qputenv("XDG_DATA_HOME", placesData.path().toUtf8());
        QFile bookmarks(placesData.filePath("user-places.xbel"));
        QVERIFY(bookmarks.open(QIODevice::WriteOnly));
        bookmarks.write(("<xbel><bookmark href=\"" + QUrl::fromLocalFile(destination.path()).toString(QUrl::FullyEncoded)
            + "\"><title>Test Place</title></bookmark></xbel>").toUtf8()); bookmarks.close();
        QVERIFY(QDir(destination.path()).mkdir("Pictures"));
        QImage landscape(256, 128, QImage::Format_RGB32); landscape.fill(QColor("#304e96"));
        {
            QPainter painter(&landscape); painter.setBrush(Qt::white); painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPoint(128, 64), 48, 48);
        }
        QVERIFY(landscape.save(destination.filePath("Landscape thumbnail with a readable filename.png")));
        QVERIFY(landscape.transformed(QTransform().rotate(90)).save(destination.filePath("Pictures/Portrait.png")));
        QFile large(destination.filePath("Large.bin")); QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(32 * 1024 * 1024)); large.close();
        QFile file(temp.filePath("select-me.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("A real text preview on a Vulkan object"); file.close();
        FileSystem files;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        engine.rootContext()->setContextProperty("xrMode", false);
        engine.rootContext()->setContextProperty("domeMode", false);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        QVERIFY(QTest::qWaitForWindowExposed(window)); QTest::qWait(500);
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Vulkan);
        auto view = window->findChild<QQuickItem *>("filesystemView"); QVERIFY(view);
        QVector3D point;
        const QVector3D center = files.data(files.index(0), FileSystem::Center).value<QVector3D>();
        const QVector3D extent = files.data(files.index(0), FileSystem::Extent).value<QVector3D>();
        QVERIFY(QMetaObject::invokeMethod(view, "mapFrom3DScene", Q_RETURN_ARG(QVector3D, point), Q_ARG(QVector3D, center + QVector3D(0, extent.y() / 2 + .1, 0))));
        const QPoint click = view->mapToScene(QPointF(point.x(), point.y())).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, click);
        QTRY_COMPARE(files.selected(), 0);
        // Sub-threshold object motion remains a click and does not rotate.
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, click);
        QMouseEvent objectDrag(QEvent::MouseMove, click + QPoint(2, 1), window->mapToGlobal(click + QPoint(2, 1)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &objectDrag);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, click + QPoint(2, 1));
        QCOMPARE(window->property("yaw").toReal(), 0.);
        QCOMPARE(window->property("pitch").toReal(), 0.);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, click);
        auto menu = window->findChild<QObject *>("contextMenu"); QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        // Item popups remain part of the same render surface for DomeD capture.
        QVERIFY(window->grabWindow().save(artifacts.filePath("context.png")));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_F2);
        auto field = window->findChild<QQuickItem *>("nameField"); QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        for (const char c : QByteArray("changed.txt")) QTest::keyClick(window, c); QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(temp.filePath("changed.txt")), 5000);
        QTRY_VERIFY(!files.busy());
        QTest::qWait(250); // Let the modal dialog close transition release its grab.
        // MMB drag changes the viewport; a short MMB click enables scroll zoom.
        const qreal before = window->property("panX").toReal();
        QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, click);
        QTest::mouseMove(window, click + QPoint(35, 20));
        QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, click + QPoint(35, 20));
        QVERIFY(window->property("panX").toReal() != before);
        const qreal zoom = window->property("zoom").toReal();
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, click);
        auto navigation = window->findChild<QObject *>("navigationMouse"); QVERIFY(navigation);
        QVERIFY(navigation->property("scrollMode").toBool());
        // Deliver without a native cursor warp; a compositor can send unrelated
        // cursor motion after a synthesized QTest::mouseMove on Wayland.
        const QPointF moved = click - QPoint(0, 80);
        QMouseEvent motion(QEvent::MouseMove, moved, window->mapToGlobal(moved), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &motion);
        QVERIFY(window->property("zoom").toReal() > zoom);
        QTest::keyClick(window, Qt::Key_Escape);
        const QPoint background = view->mapToScene(QPointF(12, 12)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, background);
        QMouseEvent tilt(QEvent::MouseMove, background + QPoint(60, 40), window->mapToGlobal(background + QPoint(60, 40)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &tilt);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, background + QPoint(60, 40));
        QVERIFY(window->property("yaw").toReal() > 0);
        QVERIFY(window->property("pitch").toReal() > 0);
        QCOMPARE(files.selected(), -1);
        const auto image = window->grabWindow(); QVERIFY(!image.isNull());
        QVERIFY(image.save("fsvr-desktop.png"));
        auto place = visualItem(window->contentItem(), "place0"); QVERIFY(place);
        QVERIFY(place->isVisible());
        const auto placePoint = place->mapToScene(QPointF(place->width() / 2, place->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, placePoint);
        QTRY_COMPARE(files.root(), destination.path());
        QTRY_COMPARE(files.rowCount(), 4);
        window->setProperty("zoom", 1);
        window->setProperty("panX", 0);
        window->setProperty("panZ", 0);
        window->setProperty("yaw", 0);
        window->setProperty("pitch", 0);
        QTest::qWait(500);
        QVERIFY(window->grabWindow().save("fsvr-map-preview.png"));
    }
#if FSV_HAS_XR
    void spatialPreviewSelectionLifecycle() {
        QTemporaryDir temp;
        QFile file(temp.filePath("preview.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Spatial preview"); file.close();
        FileSystem files;
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        QQmlEngine engine;
        engine.addImageProvider("thumbnail", new ThumbnailProvider);
        engine.rootContext()->setContextProperty("files", &files);
        QQmlComponent component(&engine, QUrl("qrc:/qml/SpatialPreview.qml"));
        QScopedPointer<QObject> preview(component.create());
        QVERIFY2(preview, qPrintable(component.errorString()));
        QVERIFY(!preview->property("visible").toBool());
        files.setSelected(0); QTRY_VERIFY(preview->property("visible").toBool());
        preview->setProperty("dismissed", true); QVERIFY(!preview->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(preview.data(), "reopen"));
        QVERIFY(preview->property("visible").toBool());
        files.setSelected(-1); QVERIFY(!preview->property("visible").toBool());
    }
    void xrComponentCompiles() {
        qmlRegisterType<HandGesture>("Fsv", 1, 0, "HandGesture");
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/qml/Xr.qml"));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        for (const auto &error : component.errors()) qWarning().noquote() << error.toString();
        QCOMPARE(component.status(), QQmlComponent::Ready);
    }
#endif
};
int main(int argc, char **argv) {
    QTemporaryDir cache, data;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("QSG_RHI_BACKEND", "vulkan");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QGuiApplication app(argc, argv);
    qmlRegisterType<BranchGeometry>("Fsv", 1, 0, "BranchGeometry");
    UiTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ui.moc"
