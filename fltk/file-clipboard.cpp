// SPDX-License-Identifier: LGPL-2.1-or-later
#include "file-clipboard.h"
#include <FL/Fl.H>
#include <FL/platform.H>
#include <X11/Xatom.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>
using Files = fsv::FileList;
namespace fs = std::filesystem;
namespace {
constexpr auto uriMime = "text/uri-list", gnomeMime = "x-special/gnome-copied-files",
               cutMime = "application/x-kde-cutselection";
constexpr size_t limit = 1024 * 1024;
using Clock = std::chrono::steady_clock;
template <class T> std::future<T> ready(T value) {
  std::promise<T> p;
  p.set_value(std::move(value));
  return p.get_future();
}
std::map<std::string, std::string> payloads(const Files &files) {
  auto uri = fsv::uriList(files);
  return {{uriMime, uri},
          {gnomeMime, (files.cut ? "cut\n" : "copy\n") + uri},
          {cutMime, files.cut ? "1" : "0"},
          {"text/plain;charset=utf-8", uri},
          {"text/plain", uri},
          {"UTF8_STRING", uri}};
}
std::string readFd(int fd) {
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } closeFd{fd};
  std::string out;
  auto deadline = Clock::now() + std::chrono::seconds(4);
  char data[16384];
  while (Clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 100) < 0) {
      if (errno == EINTR)
        continue;
      throw std::runtime_error("Clipboard read failed");
    }
    if (!p.revents)
      continue;
    auto n = read(fd, data, sizeof data);
    if (n == 0)
      return out;
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR)
        continue;
      throw std::runtime_error("Clipboard read failed");
    }
    out.append(data, n);
    if (out.size() > limit)
      throw std::runtime_error("Clipboard file list exceeds 1 MiB");
  }
  throw std::runtime_error("Clipboard owner did not send its file list");
}
void sendFd(int fd, std::string bytes) {
  std::thread([fd, bytes = std::move(bytes)] {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    auto deadline = Clock::now() + std::chrono::seconds(4);
    size_t n = 0;
    while (n < bytes.size() && Clock::now() < deadline) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 100) <= 0)
        continue;
      auto count = write(fd, bytes.data() + n, std::min<size_t>(16384, bytes.size() - n));
      if (count > 0)
        n += count;
      else if (errno != EINTR && errno != EAGAIN)
        break;
    }
    close(fd);
  }).detach();
}
} // namespace
struct FileClipboard::Impl {
  static std::vector<Impl *> &instances() {
    static std::vector<Impl *> all;
    return all;
  }
  static int fallback(int event) {
    if (event != FL_DND_ENTER && event != FL_DND_DRAG && event != FL_DND_RELEASE)
      return 0;
    auto below = Fl::belowmouse();
    for (auto s : instances())
      if (below && s->window.contains(below)) {
        int result = s->api.handle(event);
        if (result >= 0)
          return result;
      }
    return 0;
  }
  FileClipboard &api;
  Fl_Window &window;
  bool initialized = false;
  std::string setupError;
  Files own;
  std::map<std::string, std::string> data, dragData;
  wl_display *display = nullptr;
  wl_event_queue *queue = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  wl_pointer *pointer = nullptr;
  wl_keyboard *keyboard = nullptr;
  wl_data_device_manager *manager = nullptr;
  wl_data_device *device = nullptr;
  uint32_t serial = 0, pointerSerial = 0, dragSerial = 0;
  struct Offer {
    std::set<std::string> mimes;
    uint32_t action = 0, sourceActions = 0;
  };
  std::map<wl_data_offer *, Offer> offers;
  wl_data_offer *selection = nullptr, *dragOffer = nullptr, *receivingOffer = nullptr;
  int dragX = 0, dragY = 0, dropX = 0, dropY = 0;
  bool dragInside = false;
  bool toolkitFinishes = false, inToolkitCallback = false;
  struct Source {
    Impl *owner;
    wl_data_source *source;
    std::map<std::string, std::string> data;
  };
  std::map<wl_data_source *, std::unique_ptr<Source>> sources;
  wl_data_source *clipboardSource = nullptr;
  std::future<Files> dropRead;
  Display *x = nullptr;
  ::Window xwindow = 0, dragSource = 0, dragTarget = 0;
  bool xDragAccept = false;
  struct Read {
    std::promise<Files> promise;
    std::string mime, bytes, uri;
    bool targets = true, incr = false, kde = false, cut = false, drop = false;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(4);
  };
  std::map<Atom, std::unique_ptr<Read>> reads;
  struct Send {
    Display *display;
    ::Window target;
    Atom property, type;
    std::string data;
    size_t offset = 0;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(8);
  };
  std::vector<Send> sends;
  explicit Impl(FileClipboard &api, Fl_Window &w) : api(api), window(w) {
    if (instances().empty())
      Fl::add_handler(fallback);
    instances().push_back(this);
  }
  Atom atom(const char *name) const { return XInternAtom(x, name, False); }
  bool accepts(int px, int py) { return api.acceptsDrop && api.acceptsDrop(px, py); }
  void beginDrop() {
    if (!receivingOffer && !dropRead.valid() && dragOffer && dragInside &&
        offers[dragOffer].mimes.count(uriMime) && accepts(dragX, dragY) &&
        (offers[dragOffer].sourceActions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)) {
      dropX = dragX;
      dropY = dragY;
      receivingOffer = dragOffer;
      dropRead = receive(receivingOffer, true);
    }
  }
  void finishSource(wl_data_source *s) {
    if (clipboardSource == s)
      clipboardSource = nullptr;
    wl_data_source_destroy(s);
    sources.erase(s);
  }
  void destroyOffer(wl_data_offer *offer) {
    if (!offer)
      return;
    if (selection == offer)
      selection = nullptr;
    if (dragOffer == offer)
      dragOffer = nullptr;
    wl_data_offer_destroy(offer);
    offers.erase(offer);
  }
  wl_data_source *source(const Files &files, bool drag) {
    auto s = std::make_unique<Source>();
    s->owner = this;
    s->source = wl_data_device_manager_create_data_source(manager);
    s->data = payloads(files);
    auto raw = s.get();
    static const wl_data_source_listener listener = {
        [](void *, wl_data_source *, const char *) {},
        [](void *p, wl_data_source *, const char *mime, int32_t fd) {
          auto s = static_cast<Source *>(p);
          auto i = s->data.find(mime ? mime : "");
          if (i == s->data.end())
            close(fd);
          else
            sendFd(fd, i->second);
        },
        [](void *p, wl_data_source *s) { static_cast<Source *>(p)->owner->finishSource(s); },
        [](void *, wl_data_source *) {},
        [](void *p, wl_data_source *s) { static_cast<Source *>(p)->owner->finishSource(s); },
        [](void *, wl_data_source *, uint32_t) {}};
    wl_data_source_add_listener(raw->source, &listener, raw);
    for (auto &v : raw->data)
      if (v.first != "UTF8_STRING")
        wl_data_source_offer(raw->source, v.first.c_str());
    if (drag)
      wl_data_source_set_actions(raw->source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    auto result = raw->source;
    sources[result] = std::move(s);
    return result;
  }
  std::future<Files> receive(wl_data_offer *offer, bool drag) {
    if (!offer || !offers.count(offer))
      return ready(Files{});
    auto &mimes = offers.at(offer).mimes;
    auto mime = (!drag && mimes.count(gnomeMime)) ? gnomeMime : mimes.count(uriMime) ? uriMime : "";
    if (!*mime)
      return ready(Files{});
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK))
      throw std::runtime_error("Cannot receive clipboard files");
    wl_data_offer_receive(offer, mime, pipes[1]);
    close(pipes[1]);
    int cutFd = -1;
    if (!drag && mime == uriMime && mimes.count(cutMime)) {
      int cutPipe[2];
      if (!pipe2(cutPipe, O_CLOEXEC | O_NONBLOCK)) {
        wl_data_offer_receive(offer, cutMime, cutPipe[1]);
        close(cutPipe[1]);
        cutFd = cutPipe[0];
      }
    }
    wl_display_flush(display);
    return std::async(std::launch::async,
                      [fd = pipes[0], cutFd, mime = std::string(mime), drag]() mutable {
                        std::string cut;
                        try {
                          auto bytes = readFd(fd);
                          if (cutFd >= 0) {
                            int other = cutFd;
                            cutFd = -1;
                            cut = readFd(other);
                          }
                          auto result = fsv::parseFiles(bytes, mime);
                          if (cut == "1")
                            result.cut = true;
                          if (drag)
                            result.cut = false;
                          return result;
                        } catch (...) {
                          if (cutFd >= 0)
                            close(cutFd);
                          throw;
                        }
                      });
  }
  void position() {
    if (!dragOffer)
      return;
    bool valid = dragInside && offers[dragOffer].mimes.count(uriMime) && accepts(dragX, dragY);
    if (valid) {
      wl_data_offer_accept(dragOffer, dragSerial, uriMime);
      wl_data_offer_set_actions(dragOffer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
  }
  void setupWayland() {
    queue = wl_display_create_queue(display);
    registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);
    static const wl_registry_listener globals = {
        [](void *p, wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
          auto &s = *static_cast<Impl *>(p);
          if (!strcmp(iface, "wl_seat") && !s.seat)
            s.seat = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
          if (!strcmp(iface, "wl_data_device_manager") && version >= 3)
            s.manager = static_cast<wl_data_device_manager *>(
                wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
        },
        [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry, &globals, this);
    wl_display_roundtrip_queue(display, queue);
    if (!seat || !manager)
      throw std::runtime_error("Wayland file transfers require wl_data_device_manager version 3");
    static const wl_seat_listener seats = {
        [](void *p, wl_seat *, uint32_t capabilities) {
          auto &s = *static_cast<Impl *>(p);
          if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !s.pointer) {
            s.pointer = wl_seat_get_pointer(s.seat);
            static const wl_pointer_listener pointerListener = {
                [](void *p, wl_pointer *, uint32_t serial, wl_surface *, wl_fixed_t, wl_fixed_t) {
                  static_cast<Impl *>(p)->serial = serial;
                },
                [](void *, wl_pointer *, uint32_t, wl_surface *) {},
                [](void *, wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {},
                [](void *p, wl_pointer *, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
                  auto &s = *static_cast<Impl *>(p);
                  s.serial = serial;
                  if (state == WL_POINTER_BUTTON_STATE_PRESSED)
                    s.pointerSerial = serial;
                },
                [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {},
                [](void *, wl_pointer *) {},
                [](void *, wl_pointer *, uint32_t) {},
                [](void *, wl_pointer *, uint32_t, uint32_t) {},
                [](void *, wl_pointer *, uint32_t, int32_t) {},
                nullptr,
                nullptr};
            wl_pointer_add_listener(s.pointer, &pointerListener, &s);
          }
          if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !s.keyboard) {
            s.keyboard = wl_seat_get_keyboard(s.seat);
            static const wl_keyboard_listener keys = {
                [](void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) { close(fd); },
                [](void *p, wl_keyboard *, uint32_t serial, wl_surface *, wl_array *) {
                  static_cast<Impl *>(p)->serial = serial;
                },
                [](void *, wl_keyboard *, uint32_t, wl_surface *) {},
                [](void *p, wl_keyboard *, uint32_t serial, uint32_t, uint32_t, uint32_t) {
                  static_cast<Impl *>(p)->serial = serial;
                },
                [](void *p, wl_keyboard *, uint32_t serial, uint32_t, uint32_t, uint32_t,
                   uint32_t) { static_cast<Impl *>(p)->serial = serial; },
                [](void *, wl_keyboard *, int32_t, int32_t) {}};
            wl_keyboard_add_listener(s.keyboard, &keys, &s);
          }
        },
        [](void *, wl_seat *, const char *) {}};
    wl_seat_add_listener(seat, &seats, this);
    device = wl_data_device_manager_get_data_device(manager, seat);
    static const wl_data_offer_listener offerListener = {
        [](void *p, wl_data_offer *o, const char *mime) {
          static_cast<Impl *>(p)->offers[o].mimes.insert(mime);
        },
        [](void *p, wl_data_offer *o, uint32_t actions) {
          static_cast<Impl *>(p)->offers[o].sourceActions = actions;
        },
        [](void *p, wl_data_offer *o, uint32_t action) {
          static_cast<Impl *>(p)->offers[o].action = action;
        }};
    static const wl_data_device_listener devices = {
        [](void *p, wl_data_device *, wl_data_offer *o) {
          auto &s = *static_cast<Impl *>(p);
          s.offers.emplace(o, Offer{});
          wl_data_offer_add_listener(o, &offerListener, &s);
        },
        [](void *p, wl_data_device *, uint32_t serial, wl_surface *surface, wl_fixed_t x,
           wl_fixed_t y, wl_data_offer *offer) {
          auto &s = *static_cast<Impl *>(p);
          s.dragSerial = serial;
          s.dragOffer = offer;
          s.dragInside = surface == fl_wl_surface(fl_wl_xid(&s.window));
          s.dragX = wl_fixed_to_double(x) / Fl::screen_scale(s.window.screen_num());
          s.dragY = wl_fixed_to_double(y) / Fl::screen_scale(s.window.screen_num());
          s.position();
        },
        [](void *p, wl_data_device *) {
          auto &s = *static_cast<Impl *>(p);
          if (s.dragOffer && s.dragOffer != s.receivingOffer)
            s.destroyOffer(s.dragOffer);
          s.dragInside = false;
        },
        [](void *p, wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
          auto &s = *static_cast<Impl *>(p);
          s.dragX = wl_fixed_to_double(x) / Fl::screen_scale(s.window.screen_num());
          s.dragY = wl_fixed_to_double(y) / Fl::screen_scale(s.window.screen_num());
          s.position();
        },
        [](void *p, wl_data_device *) {
          auto &s = *static_cast<Impl *>(p);
          s.beginDrop();
        },
        [](void *p, wl_data_device *, wl_data_offer *offer) {
          auto &s = *static_cast<Impl *>(p);
          if (s.selection && s.selection != offer && s.selection != s.dragOffer &&
              s.selection != s.receivingOffer)
            s.destroyOffer(s.selection);
          s.selection = offer;
        }};
    wl_data_device_add_listener(device, &devices, this);
    wl_display_roundtrip_queue(display, queue);
  }
  void setup() {
    if (!setupError.empty())
      throw std::runtime_error(setupError);
    if (initialized)
      return;
    initialized = true;
    try {
      display = fl_wl_display();
      if (display) {
        setupWayland();
        return;
      }
      x = XOpenDisplay(nullptr);
      if (!x)
        throw std::runtime_error("No file clipboard display is available");
      xwindow = XCreateSimpleWindow(x, DefaultRootWindow(x), 0, 0, 1, 1, 0, 0, 0);
      XSelectInput(x, xwindow, PropertyChangeMask);
      bool haveHandler = std::any_of(instances().begin(), instances().end(),
                                     [this](auto s) { return s != this && s->x; });
      if (!haveHandler)
        Fl::add_system_handler(systemEvent, nullptr);
    } catch (const std::exception &e) {
      setupError = e.what();
      throw;
    }
  }
  void copy(const Files &files) {
    setup();
    poll();
    auto next = payloads(files);
    if (display) {
      if (!serial)
        throw std::runtime_error("Focus or click FSVR before copying files");
      auto s = files.paths.empty() ? nullptr : source(files, false);
      clipboardSource = s;
      wl_data_device_set_selection(device, files.paths.empty() ? nullptr : s, serial);
      wl_display_flush(display);
    } else {
      XSetSelectionOwner(x, atom("CLIPBOARD"), files.paths.empty() ? 0 : xwindow, CurrentTime);
      XFlush(x);
    }
    own = files;
    data = std::move(next);
  }
  void respond(Display *d, const XSelectionRequestEvent &r,
               const std::map<std::string, std::string> &content) {
    Atom property = r.property ? r.property : r.target;
    XEvent response{};
    auto &e = response.xselection;
    e.type = SelectionNotify;
    e.display = d;
    e.requestor = r.requestor;
    e.selection = r.selection;
    e.target = r.target;
    e.time = r.time;
    e.property = 0;
    auto targets = XInternAtom(d, "TARGETS", False);
    auto name = XGetAtomName(d, r.target);
    std::string mime = name ? name : "";
    if (name)
      XFree(name);
    if (r.target == targets) {
      std::vector<Atom> types{targets};
      for (auto &v : content)
        types.push_back(XInternAtom(d, v.first.c_str(), False));
      XChangeProperty(d, r.requestor, property, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(types.data()), types.size());
      e.property = property;
    } else if (auto i = content.find(mime); i != content.end()) {
      if (i->second.size() <= 32768)
        XChangeProperty(d, r.requestor, property, r.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(i->second.data()),
                        i->second.size());
      else {
        unsigned long length = i->second.size();
        XSelectInput(d, r.requestor, PropertyChangeMask);
        XChangeProperty(d, r.requestor, property, XInternAtom(d, "INCR", False), 32,
                        PropModeReplace, reinterpret_cast<unsigned char *>(&length), 1);
        sends.push_back({d, r.requestor, property, r.target, i->second});
      }
      e.property = property;
    }
    XSendEvent(d, r.requestor, False, 0, &response);
    XFlush(d);
  }
  void sendChunk(Display *d, const XPropertyEvent &p) {
    if (p.state != PropertyDelete)
      return;
    for (auto i = sends.begin(); i != sends.end(); ++i)
      if (i->display == d && i->target == p.window && i->property == p.atom) {
        auto n = std::min<size_t>(16384, i->data.size() - i->offset);
        XChangeProperty(d, i->target, i->property, i->type, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(i->data.data() + i->offset), n);
        i->offset += n;
        XFlush(d);
        if (!n)
          sends.erase(i);
        return;
      }
  }
  void convert(Atom property, Read &r, Atom target) {
    XConvertSelection(x, r.drop ? atom("XdndSelection") : atom("CLIPBOARD"), target, property,
                      xwindow, CurrentTime);
    XFlush(x);
    r.deadline = Clock::now() + std::chrono::seconds(4);
  }
  std::future<Files> readX(bool drop = false) {
    Atom property = atom(drop ? "_FSVR_DROP_FILES" : "_FSVR_CLIPBOARD_FILES");
    if (reads.count(property))
      throw std::runtime_error("Wait for the current clipboard transfer");
    auto read = std::make_unique<Read>();
    read->drop = drop;
    auto result = read->promise.get_future();
    if (drop) {
      read->targets = false;
      read->mime = uriMime;
    }
    convert(property, *read, drop ? atom(uriMime) : atom("TARGETS"));
    reads[property] = std::move(read);
    return result;
  }
  void completeRead(Atom property, Read &r) {
    if (r.mime == cutMime) {
      auto result = fsv::parseFiles(r.uri, uriMime);
      result.cut = r.bytes == "1";
      r.promise.set_value(std::move(result));
      reads.erase(property);
      return;
    }
    if (r.kde) {
      r.uri = std::move(r.bytes);
      r.bytes.clear();
      r.mime = cutMime;
      convert(property, r, atom(cutMime));
      return;
    }
    auto result = fsv::parseFiles(r.bytes, r.mime);
    if (r.drop)
      result.cut = false;
    r.promise.set_value(std::move(result));
    reads.erase(property);
  }
  void property(Atom property, bool incremental) {
    auto found = reads.find(property);
    if (found == reads.end())
      return;
    auto &r = *found->second;
    Atom type = 0;
    int format = 0;
    unsigned long n = 0, remaining = 0;
    unsigned char *bytes = nullptr;
    XGetWindowProperty(x, xwindow, property, 0, limit / 4 + 1, True, AnyPropertyType, &type,
                       &format, &n, &remaining, &bytes);
    struct Free {
      unsigned char *p;
      ~Free() {
        if (p)
          XFree(p);
      }
    } free{bytes};
    if (remaining || n > limit)
      throw std::runtime_error("Clipboard file list exceeds 1 MiB");
    if (type == atom("INCR")) {
      r.incr = true;
      XDeleteProperty(x, xwindow, property);
      XFlush(x);
      return;
    }
    if (r.targets) {
      std::set<Atom> targets;
      if (format == 32 && bytes) {
        auto a = reinterpret_cast<Atom *>(bytes);
        targets.insert(a, a + n);
      }
      r.targets = false;
      r.mime = targets.count(atom(gnomeMime)) ? gnomeMime : uriMime;
      r.kde = r.mime == uriMime && targets.count(atom(cutMime));
      convert(property, r, atom(r.mime.c_str()));
      return;
    }
    if (format != 8 && n)
      throw std::runtime_error("Invalid clipboard file data");
    if (bytes)
      r.bytes.append(reinterpret_cast<char *>(bytes), n);
    if (r.bytes.size() > limit)
      throw std::runtime_error("Clipboard file list exceeds 1 MiB");
    if (!incremental || n == 0) {
      r.incr = false;
      completeRead(property, r);
    }
    XFlush(x);
  }
  void message(::Window target, Atom type, long a, long b = 0, long c = 0, long d = 0, long e = 0) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = target;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = a;
    event.xclient.data.l[1] = b;
    event.xclient.data.l[2] = c;
    event.xclient.data.l[3] = d;
    event.xclient.data.l[4] = e;
    XSendEvent(x, target, False, NoEventMask, &event);
    XFlush(x);
  }
  static int systemEvent(void *event, void *) {
    for (auto s : instances())
      if (s->x && s->xEvent(event))
        return 1;
    return 0;
  }
  int xEvent(void *event) {
    auto &s = *this;
    auto &e = *static_cast<XEvent *>(event);
    if (e.type == PropertyNotify)
      s.sendChunk(fl_display, e.xproperty);
    if (e.type == SelectionRequest && e.xselectionrequest.selection == s.atom("XdndSelection") &&
        !s.dragData.empty()) {
      s.respond(fl_display, e.xselectionrequest, s.dragData);
      return 1;
    }
    if (e.type != ClientMessage || e.xclient.window != fl_xid(&s.window))
      return 0;
    auto &m = e.xclient;
    if (m.message_type == s.atom("XdndEnter")) {
      s.dragSource = m.data.l[0];
      s.dragTarget = m.window;
      std::set<Atom> types;
      if (m.data.l[1] & 1) {
        Atom type;
        int format;
        unsigned long n, remaining;
        unsigned char *bytes = nullptr;
        XGetWindowProperty(s.x, s.dragSource, s.atom("XdndTypeList"), 0, 1024, False, XA_ATOM,
                           &type, &format, &n, &remaining, &bytes);
        if (bytes && format == 32) {
          auto a = reinterpret_cast<Atom *>(bytes);
          types.insert(a, a + n);
        }
        if (bytes)
          XFree(bytes);
      } else
        for (int i = 2; i < 5; ++i)
          types.insert(m.data.l[i]);
      s.xDragAccept = types.count(s.atom(uriMime));
      if (!s.xDragAccept)
        s.dragSource = 0;
      return s.xDragAccept ? 1 : 0;
    }
    if (m.message_type == s.atom("XdndPosition") && s.dragSource) {
      int px = int16_t((m.data.l[2] >> 16) & 0xffff), py = int16_t(m.data.l[2] & 0xffff), tx = 0,
          ty = 0;
      ::Window child;
      XTranslateCoordinates(s.x, DefaultRootWindow(s.x), m.window, px, py, &tx, &ty, &child);
      XWindowAttributes attributes{};
      XGetWindowAttributes(s.x, m.window, &attributes);
      s.dragX = tx * s.window.w() / std::max(1, attributes.width);
      s.dragY = ty * s.window.h() / std::max(1, attributes.height);
      bool okay = s.xDragAccept && s.accepts(s.dragX, s.dragY);
      s.message(s.dragSource, s.atom("XdndStatus"), m.window, okay ? 3 : 2, 0, 0,
                okay ? s.atom("XdndActionCopy") : 0);
      return 1;
    }
    if (m.message_type == s.atom("XdndLeave") && s.dragSource) {
      s.dragSource = 0;
      return 1;
    }
    if (m.message_type == s.atom("XdndDrop") && s.dragSource) {
      if (s.xDragAccept && s.accepts(s.dragX, s.dragY)) {
        s.dropX = s.dragX;
        s.dropY = s.dragY;
        s.dropRead = s.readX(true);
      } else {
        s.message(s.dragSource, s.atom("XdndFinished"), m.window, 0);
        s.dragSource = 0;
      }
      return 1;
    }
    return 0;
  }
  std::future<Files> paste() {
    setup();
    if (display)
      wl_display_roundtrip_queue(display, queue);
    poll();
    if (display) {
      if (clipboardSource)
        return ready(own);
      return receive(selection, false);
    }
    if (XGetSelectionOwner(x, atom("CLIPBOARD")) == xwindow)
      return ready(own);
    return readX();
  }
  void poll() {
    if (!setupError.empty())
      throw std::runtime_error(setupError);
    if (!initialized) {
      if (!window.shown())
        return;
      setup();
    }
    if (display)
      wl_display_dispatch_queue_pending(display, queue);
    else
      while (XPending(x)) {
        XEvent e;
        XNextEvent(x, &e);
        Atom property = 0;
        try {
          if (e.type == SelectionRequest)
            respond(x, e.xselectionrequest, data);
          if (e.type == PropertyNotify) {
            sendChunk(x, e.xproperty);
            auto i = reads.find(e.xproperty.atom);
            if (e.xproperty.window == xwindow && e.xproperty.state == PropertyNewValue &&
                i != reads.end() && i->second->incr) {
              property = e.xproperty.atom;
              this->property(property, true);
            }
          }
          if (e.type == SelectionNotify) {
            property = e.xselection.property;
            if (property)
              this->property(property, false);
            else {
              auto property =
                  atom(e.xselection.selection == atom("XdndSelection") ? "_FSVR_DROP_FILES"
                                                                       : "_FSVR_CLIPBOARD_FILES");
              auto i = reads.find(property);
              if (i != reads.end()) {
                i->second->promise.set_value(Files{});
                reads.erase(i);
              }
            }
          }
        } catch (...) {
          auto i = reads.find(property);
          if (i != reads.end()) {
            i->second->promise.set_exception(std::current_exception());
            reads.erase(i);
          }
        }
      }
    for (auto i = reads.begin(); i != reads.end();)
      if (Clock::now() > i->second->deadline) {
        i->second->promise.set_exception(
            std::make_exception_ptr(std::runtime_error("Clipboard owner did not respond")));
        i = reads.erase(i);
      } else
        ++i;
    sends.erase(std::remove_if(sends.begin(), sends.end(),
                               [](auto &s) { return Clock::now() > s.deadline; }),
                sends.end());
    if (!inToolkitCallback && dropRead.valid() &&
        dropRead.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      bool okay = false;
      try {
        auto files = dropRead.get();
        if (!files.paths.empty() && api.dropped) {
          api.dropped(std::move(files), dropX, dropY);
          okay = true;
        }
      } catch (const std::exception &e) {
        if (api.failed)
          api.failed(e.what());
      }
      if (receivingOffer) {
        if (okay && !toolkitFinishes)
          wl_data_offer_finish(receivingOffer);
        destroyOffer(receivingOffer);
        receivingOffer = nullptr;
        toolkitFinishes = false;
      }
      if (x && dragSource) {
        message(dragSource, atom("XdndFinished"), dragTarget, okay ? 1 : 0,
                okay ? atom("XdndActionCopy") : 0);
        dragSource = 0;
      }
    }
    if (display)
      wl_display_flush(display);
  }
  bool drag(const Files &files) {
    setup();
    // The seat's newest button serial may still be waiting on our private queue.
    if (display)
      wl_display_roundtrip_queue(display, queue);
    poll();
    Files copied = files;
    copied.cut = false;
    if (display) {
      if (!pointerSerial || !window.shown())
        return false;
      auto s = source(copied, true);
      wl_data_device_start_drag(device, s, fl_wl_surface(fl_wl_xid(&window)), nullptr,
                                pointerSerial);
      wl_display_flush(display);
      return true;
    }
    dragData = payloads(copied);
    auto bytes = fsv::uriList(copied);
    Fl::copy(bytes.data(), bytes.size(), 0);
    return Fl::dnd();
  }
  ~Impl() {
    auto &all = instances();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    if (all.empty())
      Fl::remove_handler(fallback);
    if (x) {
      if (std::none_of(all.begin(), all.end(), [](auto s) { return s->x; }))
        Fl::remove_system_handler(systemEvent);
      XDestroyWindow(x, xwindow);
      XCloseDisplay(x);
    }
    if (display) {
      for (auto &s : sources)
        wl_data_source_destroy(s.first);
      for (auto &o : offers)
        wl_data_offer_destroy(o.first);
      if (device)
        wl_data_device_release(device);
      if (keyboard)
        wl_keyboard_release(keyboard);
      if (pointer)
        wl_pointer_release(pointer);
      if (seat)
        wl_seat_release(seat);
      if (manager)
        wl_data_device_manager_destroy(manager);
      if (registry)
        wl_registry_destroy(registry);
      if (queue)
        wl_event_queue_destroy(queue);
      wl_display_flush(display);
    }
  }
};
FileClipboard::FileClipboard(Fl_Window &w) : impl(std::make_unique<Impl>(*this, w)) {}
FileClipboard::~FileClipboard() = default;
void FileClipboard::poll() { impl->poll(); }
int FileClipboard::handle(int event) {
  if (event == FL_PASTE && impl->toolkitFinishes)
    return 1;
  if (!impl->display || !impl->setupError.empty() ||
      (event != FL_DND_ENTER && event != FL_DND_DRAG && event != FL_DND_RELEASE))
    return -1;
  // FLTK owns another data device on the same seat. Its drop handler must finish
  // the transaction, while this adapter receives the original URI bytes first.
  // Otherwise its rejected offer can cancel the shared source before we read it.
  if (event == FL_DND_RELEASE)
    impl->toolkitFinishes = true;
  impl->inToolkitCallback = true;
  struct Reset {
    bool &flag;
    ~Reset() { flag = false; }
  } reset{impl->inToolkitCallback};
  impl->poll();
  bool valid = impl->dragInside && impl->dragOffer &&
               impl->offers[impl->dragOffer].mimes.count(uriMime) &&
               impl->accepts(Fl::event_x(), Fl::event_y());
  if (valid) {
    Fl::belowmouse(&impl->window);
    if (event == FL_DND_RELEASE)
      impl->beginDrop();
  }
  if (event == FL_DND_RELEASE && impl->receivingOffer) {
    wl_display_roundtrip_queue(impl->display, impl->queue);
    return 1;
  }
  if (!valid) {
    if (event == FL_DND_RELEASE)
      impl->toolkitFinishes = false;
    return -1;
  }
  return 1;
}
int FileClipboard::filter(int event, Fl_Window *window) {
  for (auto s : Impl::instances())
    if (&s->window == window)
      return s->api.handle(event);
  return -1;
}
void FileClipboard::copy(const Files &files) { impl->copy(files); }
std::future<Files> FileClipboard::paste() { return impl->paste(); }
void FileClipboard::moved(const Files &files, const std::vector<fs::path> &completed) {
  if (impl->display)
    wl_display_roundtrip_queue(impl->display, impl->queue);
  impl->poll();
  if (!files.cut || impl->own.paths != files.paths)
    return;
  if (impl->display ? !impl->clipboardSource
                    : XGetSelectionOwner(impl->x, impl->atom("CLIPBOARD")) != impl->xwindow)
    return;
  auto remaining = files;
  for (auto &p : completed)
    remaining.paths.erase(std::remove(remaining.paths.begin(), remaining.paths.end(), p),
                          remaining.paths.end());
  impl->copy(remaining);
}
bool FileClipboard::drag(const Files &files) { return impl->drag(files); }
