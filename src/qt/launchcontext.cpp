// SPDX-License-Identifier: LGPL-2.1-or-later
#include "launchcontext.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#endif

bool LaunchContext::spatialForSocket(const QString &socketPath, qint64 compositorPid)
{
    const QFileInfo info(socketPath + ".dome-session.json");
    if (!info.isFile() || info.isSymLink() || info.size() > 4096) return false;
#if defined(Q_OS_LINUX)
    if (info.ownerId() != getuid() || (info.permissions() & (QFile::WriteGroup | QFile::WriteOther))) return false;
#endif
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto object = QJsonDocument::fromJson(file.read(4097)).object();
    return object.value("version").toInt() == 1
        && object.value("compositor_pid").toInteger() == compositorPid
        && object.value("realm").toString() == "xr";
}
LaunchContext LaunchContext::detect()
{
#if defined(Q_OS_LINUX)
    auto identify = [](int fd) -> LaunchContext {
        ucred peer{};
        socklen_t length = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || peer.uid != getuid()
            || QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(peer.pid))).fileName()
                != QStringLiteral("dome-compositor")) return {};
        // getpeername works for inherited WAYLAND_SOCKET FDs too. Environment
        // hints and headset availability cannot identify the launch realm.
        sockaddr_un address{};
        length = sizeof(address);
        const bool named = getpeername(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0
            && address.sun_family == AF_UNIX && address.sun_path[0];
        const QString socket = named ? QFile::decodeName(QByteArray(address.sun_path,
            strnlen(address.sun_path, sizeof(address.sun_path)))) : QString();
        return {true, !socket.isEmpty() && spatialForSocket(socket, peer.pid)};
    };
    bool inherited = false;
    const int inheritedFd = qEnvironmentVariableIntValue("WAYLAND_SOCKET", &inherited);
    if (inherited && inheritedFd >= 0) return identify(inheritedFd);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (display.isEmpty()) return {};
    const QByteArray path = QFile::encodeName(QDir::isAbsolutePath(display)
        ? display : QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).filePath(display));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= qsizetype(sizeof(address.sun_path))) return {};
    std::memcpy(address.sun_path, path.constData(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return {};
    const auto context = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0
        ? identify(fd) : LaunchContext{};
    close(fd);
    return context;
#else
    return {};
#endif
}
