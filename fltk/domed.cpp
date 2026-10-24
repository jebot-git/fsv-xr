// SPDX-License-Identifier: MIT
#include "domed.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
namespace fsv_stereo {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Fd {
  int value = -1;
  explicit Fd(int v) : value(v) {}
  ~Fd() {
    if (value >= 0)
      ::close(value);
  }
  operator int() const { return value; }
};
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void ready(int fd, short events) {
  pollfd p{fd, events, 0};
  int result;
  do {
    result = poll(&p, 1, 120);
  } while (result < 0 && errno == EINTR);
  require(result == 1 && (p.revents & events),
          "DomeD control connection timed out");
}
void connected(int fd, const sockaddr *address, socklen_t length) {
  int result = connect(fd, address, length);
  if (result < 0) {
    require(errno == EINPROGRESS, "DomeD connection unavailable");
    ready(fd, POLLOUT);
    int error = 0;
    socklen_t n = sizeof(error);
    require(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) == 0 && error == 0,
            "DomeD connection refused");
  }
}
pid_t owner(const fs::path &path) {
  Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  require(fd >= 0, "Cannot open compositor socket");
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  auto text = path.string();
  require(text.size() < sizeof(a.sun_path), "Compositor socket path too long");
  std::copy(text.begin(), text.end(), a.sun_path);
  connected(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  ucred credential{};
  socklen_t n = sizeof(credential);
  require(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &n) == 0 &&
              credential.uid == getuid(),
          "Compositor socket has a different owner");
  auto name =
      fs::read_symlink("/proc/" + std::to_string(credential.pid) + "/exe")
          .string();
  const std::string deleted = " (deleted)";
  if (name.size() > deleted.size() &&
      name.compare(name.size() - deleted.size(), deleted.size(), deleted) == 0)
    name.resize(name.size() - deleted.size());
  require(fs::path(name).filename() == "dome-compositor",
          "Wayland socket is not DomeD");
  return credential.pid;
}
Json record(const fs::path &path) {
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat st{};
  require(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == getuid() && !(st.st_mode & 0022) && st.st_size > 0 &&
              st.st_size <= 4096,
          "Invalid DomeD session record");
  std::string text(st.st_size, '\0');
  size_t done = 0;
  while (done < text.size()) {
    auto n = read(fd, text.data() + done, text.size() - done);
    require(n > 0, "Cannot read DomeD session record");
    done += n;
  }
  return Json::parse(text);
}
Json request(unsigned port, const std::string &path, size_t limit = 8192) {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  require(fd >= 0, "Cannot open DomeD control socket");
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  connected(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  std::string query = "GET " + path +
                      " HTTP/1.0\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < query.size()) {
    ready(fd, POLLOUT);
    auto n = send(fd, query.data() + sent, query.size() - sent, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    require(n > 0, "Cannot request DomeD mode");
    sent += n;
  }
  std::string response;
  size_t bodyAt = std::string::npos, expected = 0;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
  while (std::chrono::steady_clock::now() < deadline) {
    char data[8192];
    auto n = recv(fd, data, sizeof(data), 0);
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        ready(fd, POLLIN);
        continue;
      }
      throw std::runtime_error("Cannot read DomeD response");
    }
    response.append(data, n);
    require(response.size() <= limit + 8192, "DomeD response too large");
    if (bodyAt == std::string::npos) {
      auto split = response.find("\r\n\r\n");
      if (split == std::string::npos) {
        require(response.size() <= 8192, "DomeD headers too large");
        continue;
      }
      require(response.rfind("HTTP/1.0 200", 0) == 0 ||
                  response.rfind("HTTP/1.1 200", 0) == 0,
              "DomeD request failed");
      bodyAt = split + 4;
      std::string headers = response.substr(0, split);
      std::transform(headers.begin(), headers.end(), headers.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      auto length = headers.find("\r\ncontent-length:");
      require(length != std::string::npos, "DomeD response has no length");
      expected = std::stoull(headers.substr(length + 17));
      require(expected > 0 && expected <= limit,
              "DomeD response exceeds limit");
    }
    // Parse the state once, after its complete body arrives. Re-parsing each
    // partial packet would make a large compositor scene quadratic in size.
    if (bodyAt != std::string::npos && response.size() - bodyAt >= expected)
      return Json::parse(response.substr(bodyAt, expected));
  }
  throw std::runtime_error("Incomplete DomeD response");
}

DomeMode observe() {
  DomeMode out;
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  std::string name = desktop ? desktop : "";
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  out.managed = name.find("dome") != std::string::npos;
  try {
    const char *display = getenv("WAYLAND_DISPLAY"),
               *runtime = getenv("XDG_RUNTIME_DIR");
    require(display && *display, "DomeD Wayland display is unavailable");
    fs::path socketPath = display;
    if (socketPath.is_relative()) {
      require(runtime && *runtime, "DomeD runtime directory is unavailable");
      socketPath = fs::path(runtime) / socketPath;
    }
    auto pid = owner(socketPath);
    out.managed = true;
    auto info = record(socketPath.string() + ".dome-session.json");
    require(info.value("version", 0) == 1 &&
                info.value("compositor_pid", 0) == pid,
            "Stale DomeD session record");
    require(info.contains("control_port") &&
                info["control_port"].is_number_unsigned(),
            "Invalid DomeD control port");
    auto port = info["control_port"].get<unsigned>();
    require(port > 0 && port <= 65535, "Invalid DomeD control port");
    auto value = request(port, "/api/xr-control");
    auto state = request(port, "/api/state", 2 * 1024 * 1024);
    auto scene = state.at("scene");
    int lane = 1;
    uint64_t windowId = 0;
    for (const auto &window : scene.value("windows", nlohmann::json::array())) {
      if (window.value("app_id", "") == "org.fsvxr.fsvr.Sbs" &&
          window.value("client_pid", 0) == getpid() &&
          window.value("workspace", 0) == scene.value("active_workspace", 0)) {
        lane = std::clamp(window.value("lane", 1), 0, 2);
        windowId = window.at("id").get<uint64_t>();
        out.windowId = windowId;
        if (window.at("id") == scene.value("focused", Json()))
          break;
      }
    }
    if (state.contains("spans") && state["spans"].size() == 3) {
      const auto &span = state["spans"][lane];
      float degrees = span[1].get<float>() - span[0].get<float>();
      if (std::isfinite(degrees) && degrees > 0 && degrees < 180)
        out.laneHeightRadians = degrees * .01745329252f;
    }
    auto distance = scene.at("config").at("pullout_distance_m").get<float>();
    require(std::isfinite(distance) && distance >= .6f && distance <= 3.f,
            "Invalid DomeD pull-out distance");
    out.pulloutDistance = distance;
    if (scene.contains("pullout_anchor") &&
        !scene["pullout_anchor"].is_null()) {
      auto &anchor = scene["pullout_anchor"];
      out.pulloutPosition = anchor.at("position_m").get<std::array<float, 3>>();
      out.pulloutOrientation =
          anchor.at("orientation_xyzw").get<std::array<float, 4>>();
      for (float n : out.pulloutPosition)
        require(std::isfinite(n), "Invalid DomeD pull-out position");
      float norm = 0;
      for (float n : out.pulloutOrientation) {
        require(std::isfinite(n), "Invalid DomeD pull-out orientation");
        norm += n * n;
      }
      require(norm > 1e-6f, "Invalid DomeD pull-out quaternion");
      for (float &n : out.pulloutOrientation)
        n /= std::sqrt(norm);
      out.hasPulloutAnchor = true;
    }
    require(value.contains("visible") && value["visible"].is_boolean() &&
                value.contains("input_realm") &&
                value["input_realm"].is_string(),
            "Invalid DomeD presentation state");
    auto realm = value["input_realm"].get<std::string>();
    require(realm == "desktop" || realm == "xr", "Unknown DomeD input mode");
    out.known = true;
    out.xr = value["visible"].get<bool>() && realm == "xr" &&
             !value.value("exit_requested", false);
    if (value.value("stereo_views", false)) {
      auto stereo = request(port, "/api/integrations/fsvr", 128 * 1024);
      out.stereoSupported =
          stereo.value("version", 0) == 1 &&
          stereo.value("compositor_pid", 0) == pid &&
          stereo.value("sbs_app_id", "") == "org.fsvxr.fsvr.Sbs";
      if (out.stereoSupported) {
        out.headPosition =
            stereo.at("head").at("position_m").get<std::array<float, 3>>();
        out.headOrientation = stereo.at("head")
                                  .at("orientation_xyzw")
                                  .get<std::array<float, 4>>();
        for (float v : out.headPosition)
          require(std::isfinite(v), "Invalid head position");
        float headNorm = 0;
        for (float v : out.headOrientation) {
          require(std::isfinite(v), "Invalid head orientation");
          headNorm += v * v;
        }
        require(headNorm > .99f && headNorm < 1.01f, "Invalid head quaternion");
        if (stereo.contains("eyes") && !stereo["eyes"].is_null()) {
          for (int eye = 0; eye < 2; ++eye) {
            out.eyes[eye] = stereo["eyes"]
                                .at(eye)
                                .at("position_m")
                                .get<std::array<float, 3>>();
            for (float v : out.eyes[eye])
              require(std::isfinite(v), "Invalid eye position");
          }
          out.trackedEyes = true;
        }
        for (const auto &quad : stereo.value("quads", Json::array())) {
          if (quad.at("id").get<uint64_t>() != windowId)
            continue;
          out.windowPosition =
              quad.at("pose").at("position_m").get<std::array<float, 3>>();
          out.windowOrientation = quad.at("pose")
                                      .at("orientation_xyzw")
                                      .get<std::array<float, 4>>();
          out.windowSize = quad.at("size").get<std::array<float, 2>>();
          for (float v : out.windowPosition)
            require(std::isfinite(v), "Invalid window position");
          float norm = 0;
          for (float v : out.windowOrientation) {
            require(std::isfinite(v), "Invalid window orientation");
            norm += v * v;
          }
          require(norm > .99f && norm < 1.01f, "Invalid window quaternion");
          for (float v : out.windowSize)
            require(std::isfinite(v) && v > 0, "Invalid window extent");
          if (quad.contains("dome") && !quad["dome"].is_null()) {
            const auto &d = quad["dome"];
            out.cylinderPosition =
                d.at("frame").at("position_m").get<std::array<float, 3>>();
            out.cylinderOrientation = d.at("frame")
                                          .at("orientation_xyzw")
                                          .get<std::array<float, 4>>();
            out.cylinderRadius = d.at("radius");
            out.cylinderHorizontal = d.at("horizontal");
            out.cylinderTop = d.at("top");
            out.cylinderBottom = d.at("bottom");
            for (float v : out.cylinderPosition)
              require(std::isfinite(v), "Invalid cylinder position");
            float norm = 0;
            for (float v : out.cylinderOrientation) {
              require(std::isfinite(v), "Invalid cylinder orientation");
              norm += v * v;
            }
            require(norm > .99f && norm < 1.01f, "Invalid cylinder quaternion");
            require(std::isfinite(out.cylinderRadius) &&
                        out.cylinderRadius > .01f &&
                        std::isfinite(out.cylinderHorizontal) &&
                        out.cylinderHorizontal > 0 &&
                        out.cylinderHorizontal < 6.29f &&
                        std::isfinite(out.cylinderTop) &&
                        std::isfinite(out.cylinderBottom) &&
                        out.cylinderTop > out.cylinderBottom,
                    "Invalid cylinder dimensions");
            out.hasCylinder = true;
          }
          out.hasWindowQuad = true;
          break;
        }
      }
    }
    out.message = out.xr ? "DomeD: XR mode" : "DomeD: desktop mode";
  } catch (const std::exception &e) {
    out.known = out.xr = false;
    out.message = out.managed ? std::string("XR paused: ") + e.what()
                              : "Standalone desktop";
  }
  return out;
}
} // namespace
std::string configureLaunchRealm(const std::string &requested) {
  require(requested == "inherit" || requested == "desktop" || requested == "xr",
          "Unknown launch realm");
  const char *display = getenv("WAYLAND_DISPLAY"),
             *runtime = getenv("XDG_RUNTIME_DIR");
  if (!display || !*display) {
    require(requested == "inherit", "No DomeD launch context");
    return "standalone";
  }
  fs::path path = display;
  if (path.is_relative()) {
    require(runtime && *runtime, "Missing Wayland runtime directory");
    path = fs::path(runtime) / path;
  }
  pid_t pid;
  Json context;
  try {
    pid = owner(path);
    context = record(path.string() + ".dome-session.json");
    require(context.value("compositor_pid", 0) == pid &&
                context.value("version", 0) == 1,
            "Invalid launch context");
  } catch (const std::exception &) {
    if (requested != "inherit")
      throw;
    return "standalone";
  }
  std::string realm = context.value("realm", "");
  require(realm == "desktop" || realm == "xr", "Invalid launch realm");
  if (requested != "inherit" && realm != requested) {
    bool found = false;
    size_t examined = 0;
    for (auto &entry : fs::directory_iterator(path.parent_path())) {
      if (++examined > 256)
        break;
      auto filename = entry.path().string();
      const std::string suffix = ".dome-session.json";
      if (filename.size() < suffix.size() ||
          filename.compare(filename.size() - suffix.size(), suffix.size(),
                           suffix) != 0)
        continue;
      try {
        auto info = record(entry.path());
        fs::path socket = filename.substr(0, filename.size() - suffix.size());
        if (info.value("compositor_pid", 0) == pid &&
            info.value("version", 0) == 1 &&
            info.value("realm", "") == requested && owner(socket) == pid) {
          path = socket;
          realm = requested;
          found = true;
          break;
        }
      } catch (const std::exception &) {
      }
    }
    require(found, "Requested DomeD compositor realm is unavailable");
  }
  setenv("WAYLAND_DISPLAY", path.c_str(), 1);
  unsetenv("WAYLAND_SOCKET");
  // Native DomeD windows must use the socket's realm, including when a parent
  // process carries a stale X11 toolkit preference.
  setenv("FLTK_BACKEND", "wayland", 1);
  return realm;
}
struct DomeMonitor::State {
  mutable std::mutex mutex;
  DomeMode mode;
  std::atomic_bool stop{false};
  std::chrono::steady_clock::time_point updated{};
};
DomeMonitor::DomeMonitor() : state(std::make_shared<State>()) {
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  std::string d = desktop ? desktop : "";
  std::transform(d.begin(), d.end(), d.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  state->mode.managed = d.find("dome") != std::string::npos;
  if (const char *display = getenv("WAYLAND_DISPLAY")) {
    fs::path path = display;
    if (path.is_relative()) {
      if (const char *runtime = getenv("XDG_RUNTIME_DIR"))
        path = fs::path(runtime) / path;
    }
    std::error_code error;
    if (fs::exists(path.string() + ".dome-session.json", error))
      state->mode.managed = true;
  }
  auto s = state;
  std::thread([s] {
    while (!s->stop) {
      auto mode = observe();
      bool fast = mode.xr && mode.stereoSupported;
      {
        std::lock_guard lock(s->mutex);
        s->mode = std::move(mode);
        s->updated = std::chrono::steady_clock::now();
      }
      for (int i = 0; i < (fast ? 1 : 10) && !s->stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }).detach();
}
DomeMonitor::~DomeMonitor() { state->stop = true; }
DomeMode DomeMonitor::snapshot() const {
  std::lock_guard lock(state->mutex);
  auto out = state->mode;
  if (out.managed && std::chrono::steady_clock::now() - state->updated >
                         std::chrono::seconds(1)) {
    out.known = false;
    out.xr = false;
    out.message = "XR paused: DomeD mode unavailable";
  }
  return out;
}
} // namespace fsv_stereo
