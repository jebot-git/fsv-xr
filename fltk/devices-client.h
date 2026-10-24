// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <gio/gio.h>

#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

// All IPC waits stay off the FLTK thread. There is one device owner per login
// bus.
class DevicesClient {
  std::shared_ptr<GCancellable> cancel{g_cancellable_new(), g_object_unref};
  std::future<std::string> snapshot, operation;
  // Promise-backed futures never wait for a worker during widget
  // destruction. The worker owns its cancellation token and arguments,
  // never the window.
  template <class Work> static std::future<std::string> background(Work work) {
    std::packaged_task<std::string()> task(std::move(work));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();
    return result;
  }
  static std::string call(const char *method, GVariant *args,
                          const std::shared_ptr<GCancellable> &cancel) {
    g_variant_ref_sink(args);
    GError *error = nullptr;
    auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel.get(), &error);
    GVariant *reply = nullptr;
    if (bus) {
      reply = g_dbus_connection_call_sync(
          bus, "org.fsvxr.Devices", "/org/fsvxr/Devices", "org.fsvxr.Devices",
          method, args, nullptr, G_DBUS_CALL_FLAGS_NONE,
          std::string(method) == "Action" ? 125000 : 5000, cancel.get(),
          &error);
      g_object_unref(bus);
    }
    g_variant_unref(args);
    if (!reply) {
      std::string message =
          error ? error->message : "Device service unavailable";
      g_clear_error(&error);
      throw std::runtime_error(message);
    }
    std::string value;
    if (g_variant_n_children(reply)) {
      const char *text;
      g_variant_get(reply, "(&s)", &text);
      value = text;
    }
    g_variant_unref(reply);
    return value;
  }

public:
  ~DevicesClient() { g_cancellable_cancel(cancel.get()); }
  void refresh() {
    if (!snapshot.valid())
      snapshot = background(
          [c = cancel] { return call("Snapshot", g_variant_new("()"), c); });
  }
  std::optional<std::string> result(bool action = false) {
    auto &future = action ? operation : snapshot;
    if (!future.valid() ||
        future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return {};
    return future.get();
  }
  bool busy() const { return operation.valid(); }
  bool act(std::string action, std::string device, std::string secret = {}) {
    if (busy())
      return false;
    auto params = g_variant_ref_sink(
        g_variant_new("(sss)", action.c_str(), device.c_str(), secret.c_str()));
    std::fill(secret.begin(), secret.end(), '\0');
    operation = background([c = cancel, params] {
      auto owned = std::shared_ptr<GVariant>(params, g_variant_unref);
      return call("Action", params, c);
    });
    return true;
  }
  bool automount(bool enabled) {
    if (busy())
      return false;
    operation = background([c = cancel, enabled] {
      return call("SetAutomount", g_variant_new("(b)", enabled), c);
    });
    return true;
  }
};
