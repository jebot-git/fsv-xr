// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "thumbnail.h"
#include "gestures.h"
#include "domed.h"
#include "appearance.h"
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
#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#endif

// DomeD launches desktop entries on its selected Wayland socket. Detect the
// actual compositor rather than assuming a socket number or desktop name.
static bool runningInDomeD()
{
#if defined(Q_OS_LINUX)
    auto isDomePeer = [](int fd) {
        ucred peer{};
        socklen_t length = sizeof(peer);
        return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0
            && peer.uid == getuid()
            && QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(peer.pid))).fileName()
                == QStringLiteral("dome-compositor");
    };
    bool hasSocket = false;
    const int inheritedSocket = qEnvironmentVariableIntValue("WAYLAND_SOCKET", &hasSocket);
    if (hasSocket && inheritedSocket >= 0) return isDomePeer(inheritedSocket);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (display.isEmpty()) return false;
    const QByteArray path = QFile::encodeName(QDir::isAbsolutePath(display)
        ? display : QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath(display));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= qsizetype(sizeof(address.sun_path))) return false;
    std::memcpy(address.sun_path, path.constData(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    const bool connected = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    const bool identified = connected && isDomePeer(fd);
    close(fd);
    return identified;
#else
    return false;
#endif
}

int main(int argc, char **argv)
{
    // Inspect inherited Wayland FDs before Qt can consume WAYLAND_SOCKET.
    const bool domeEnvironment = runningInDomeD();
    if (domeEnvironment && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "wayland");
    // Force Vulkan for both desktop RHI and Qt's OpenXR graphics binding.
    qputenv("QSG_RHI_BACKEND", "vulkan");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QApplication app(argc, argv);
    app.setApplicationName("fsv-xr");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("fsv-xr");
    app.setDesktopFileName("org.fsvxr.fsv-xr");
    QImageReader::setAllocationLimit(64);
    QCommandLineParser parser;
    parser.setApplicationDescription("Vulkan filesystem explorer. Automatically uses XR presentation inside DomeD and desktop presentation elsewhere.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"xr", "Start a standalone OpenXR session (requires an active runtime)."});
    parser.addOption({"domed", "Run as a native DomeD Wayland client with spatial file popups."});
    parser.addOption({"desktop", "Use desktop presentation even when launched inside DomeD."});
    parser.addOption({"smoke-test", "Render for three seconds, then exit (for CI)."});
    parser.addPositionalArgument("directory", "Directory to visualize", "[directory]");
    parser.process(app);
    const bool xr = parser.isSet("xr");
    if (int(xr) + int(parser.isSet("domed")) + int(parser.isSet("desktop")) > 1) {
        qCritical("--xr, --domed and --desktop are separate presentation modes."); return 2;
    }
    const bool domed = parser.isSet("domed") || (!xr && !parser.isSet("desktop")
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
    qmlRegisterType<HandGesture>("Fsv", 1, 0, "HandGesture");
    FileSystem files;
    Appearance appearance(domed || xr);
    if (domed) {
        DomeWindow window(&files, &appearance);
        if (!window.ready()) return 1;
        files.navigate(parser.positionalArguments().value(0, QDir::homePath()));
        window.show();
        if (parser.isSet("smoke-test")) QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    QQmlApplicationEngine engine;
    engine.addImageProvider("thumbnail", new ThumbnailProvider);
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.rootContext()->setContextProperty("files", &files);
    engine.rootContext()->setContextProperty("xrMode", xr || domed);
    engine.rootContext()->setContextProperty("domeMode", domed);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(xr ? "qrc:/qml/Xr.qml" : "qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    files.navigate(parser.positionalArguments().value(0, QDir::homePath()));
    if (parser.isSet("smoke-test")) QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    return app.exec();
}
