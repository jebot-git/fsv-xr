// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "thumbnail.h"
#include "gestures.h"
#include "domed.h"
#include "appearance.h"
#include "branchgeometry.h"
#include "launchcontext.h"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QCommandLineParser>
#include <QDir>
#include <QTimer>
#include <QImageReader>
#include <QQuickStyle>
#include <QFileInfo>
#include <QFile>
int main(int argc, char **argv)
{
    // Inspect inherited Wayland FDs before Qt can consume WAYLAND_SOCKET.
    const auto launchContext = LaunchContext::detect();
    const bool domeEnvironment = launchContext.dome;
    if (domeEnvironment && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "wayland");
    // Force Vulkan for both desktop RHI and Qt's OpenXR graphics binding.
    qputenv("QSG_RHI_BACKEND", "vulkan");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QApplication app(argc, argv);
    app.setApplicationName("fsvr");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("fsvr");
    app.setDesktopFileName("org.fsvxr.fsvr");
    QImageReader::setAllocationLimit(64);
    QCommandLineParser parser;
    parser.setApplicationDescription("Vulkan filesystem explorer. Uses DomeD's desktop or XR launch realm; desktop presentation elsewhere.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"xr", "Start a standalone OpenXR session (requires an active runtime)."});
    parser.addOption({"domed", "Use the DomeD Wayland bridge, preserving the socket's desktop/XR launch realm."});
    parser.addOption({"desktop", "Use desktop presentation even when launched inside DomeD."});
    parser.addOption({"smoke-test", "Render for three seconds, then exit (for CI)."});
    parser.addOption({"select", "Reveal the supplied files or directories in their parent folders."});
    parser.addOption({"properties", "Reveal the supplied items and show their properties."});
    parser.addPositionalArgument("locations", "Local paths, file: URLs or trash:/; additional locations open in tabs.", "[locations...]");
    parser.process(app);
    const bool xr = parser.isSet("xr");
    if (xr && (parser.isSet("domed") || parser.isSet("desktop"))) {
        qCritical("--xr cannot be combined with --domed or --desktop."); return 2;
    }
    if (xr && domeEnvironment) {
        qCritical("--xr is unavailable inside DomeD. Launch fsvr without --xr to use DomeD's mouse and keyboard controls."); return 2;
    }
    const bool domed = parser.isSet("domed") || (!xr
        && QGuiApplication::platformName().startsWith("wayland") && domeEnvironment);
    // Desktop-specific Quick styles (e.g. Kirigami) can ignore application
    // palettes and continue using the host desktop's colours. Fusion honours
    // DomeD's explicit palette and updates it live.
    if (xr || domed) QQuickStyle::setStyle("Fusion");
    if (domed && !QGuiApplication::platformName().startsWith("wayland")) {
        qCritical("--domed needs QT_QPA_PLATFORM=wayland and DomeD's WAYLAND_DISPLAY."); return 2;
    }
#if !FSV_HAS_XR
    if (xr) { qCritical("Built without Qt Quick 3D XR; rebuild with FSV_XR enabled."); return 2; }
#endif
    qmlRegisterType<BranchGeometry>("Fsv", 1, 0, "BranchGeometry");
    FileSystem files;
    Appearance appearance(domed || xr);
    auto openLocations = [&] {
        auto locations = parser.positionalArguments();
        if (locations.isEmpty()) locations << QDir::homePath();
        for (int i = 0; i < locations.size(); ++i)
            files.openLocation(locations[i], parser.isSet("select"), parser.isSet("properties"), i > 0);
    };
    if (domed) {
        DomeWindow window(&files, &appearance, launchContext.spatial && !parser.isSet("desktop"));
        if (!window.ready()) return 1;
        openLocations();
        window.show();
        if (parser.isSet("smoke-test")) QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    // Hand input belongs exclusively to the standalone OpenXR scene. DomeD
    // renders Browser.qml and receives only the compositor's Wayland seat input.
    if (xr) qmlRegisterType<HandGesture>("Fsv", 1, 0, "HandGesture");
    QQmlApplicationEngine engine;
    engine.addImageProvider("thumbnail", new ThumbnailProvider);
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.rootContext()->setContextProperty("files", &files);
    engine.rootContext()->setContextProperty("xrMode", xr);
    engine.rootContext()->setContextProperty("domeMode", domed);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(xr ? "qrc:/qml/Xr.qml" : "qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    openLocations();
    if (parser.isSet("smoke-test")) QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    return app.exec();
}
