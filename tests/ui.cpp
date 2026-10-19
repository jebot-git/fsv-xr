// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "thumbnail.h"
#include "gestures.h"
#include <QtTest>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QFile>
#include <QPainter>

class UiTest : public QObject {
    Q_OBJECT
    static QQuickItem *visualItem(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems()) if (auto *found = visualItem(child, name)) return found;
        return nullptr;
    }
private slots:
    void zoomKeepsSelectedItemAnchored() {
        QTemporaryDir temp;
        for (const auto &name : {"left.txt", "right.txt"}) {
            QFile file(temp.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("anchor");
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
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("yaw", 31.); window->setProperty("pitch", 18.);
        window->setProperty("panX", 7.); window->setProperty("panY", 3.); window->setProperty("panZ", -5.);
        window->setProperty("zoom", 1.6);
        files.setSelected(0);
        const auto center = files.selection()["center"].value<QVector3D>();
        const auto size = files.selection()["extent"].value<QVector3D>();
        const QVector3D anchor = center + QVector3D(0, size.y()/2 + .3, 0);
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
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, point);
        QVERIFY(navigation->property("scrollMode").toBool());
        QMouseEvent move(QEvent::MouseMove, point - QPoint(0, 30), window->mapToGlobal(point - QPoint(0, 30)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        QVERIFY(window->property("zoom").toReal() > 1.6);
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, point);
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
        // Dragging an object keeps selection behaviour; only a background
        // press (or Alt) initiates view rotation.
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, click);
        QMouseEvent objectDrag(QEvent::MouseMove, click + QPoint(20, 10), window->mapToGlobal(click + QPoint(20, 10)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &objectDrag);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, click + QPoint(20, 10));
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
        QVERIFY(image.save("fsv-xr-desktop.png"));
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
        QVERIFY(window->grabWindow().save("fsv-xr-map-preview.png"));
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
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    qputenv("QSG_RHI_BACKEND", "vulkan");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QGuiApplication app(argc, argv);
    UiTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ui.moc"
