#include "stereo-compositor.h"
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <sys/prctl.h>

#include <chrono>
#include <ctime>
#include <future>
#include <iomanip>
#include <optional>
#include <sstream>

#include "appearance.h"
#include "desktop-menu.h"
#include "devices-client.h"
#include "gpu-window.h"
#include "model-preview.h"
#include "model.h"
#include "spatial-view.h"
namespace fs = std::filesystem;
class Sidebar : public Fl_Group {
public:
  Fl_Hold_Browser list{8, 80, 250, 280};
  Fl_Box devicesTitle{8, 364, 250, 22, "Devices"};
  Fl_Hold_Browser devices{8, 386, 250, 120};
  Fl_Button deviceOpen{8, 510, 80, 28, "Open"};
  Fl_Button deviceUnmount{90, 510, 80, 28, "Unmount"};
  Fl_Button deviceRemove{172, 510, 86, 28, "Remove"};
  Fl_Multiline_Output inspector{8, 550, 250, 110};
  Sidebar() : Fl_Group(8, 80, 250, 580) {
    end();
    devicesTitle.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    devices.format_char(0);
    deviceOpen.tooltip("Mount or unlock the selected device and open it");
    deviceUnmount.tooltip(
        "Unmount without forcing; busy volumes remain mounted");
    deviceRemove.tooltip("Unmount all volumes on the drive and safely eject or "
                         "power it off");
    resizable(list);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    int info = std::min(110, H / 4), media = std::min(120, H / 4);
    int files = std::max(40, H - info - media - 62);
    list.resize(X, Y, W, files);
    devicesTitle.resize(X, Y + files + 4, W, 22);
    devices.resize(X, Y + files + 26, W, media);
    int buttonY = Y + files + media + 30, bw = (W - 4) / 3;
    deviceOpen.resize(X, buttonY, bw, 28);
    deviceUnmount.resize(X + bw + 2, buttonY, bw, 28);
    deviceRemove.resize(X + 2 * bw + 4, buttonY, W - 2 * bw - 4, 28);
    inspector.resize(X, Y + H - info, W, info);
  }
};
class Browser : public dx::GpuWindow {
  fsv_stereo::DomeMonitor domeMonitor;
  fsv_stereo::Compositor stereo;
  bool stereoEnabled = true, stereoPacked = false;
  Fl_Menu_Bar menu{0, 0, 1000, 30};
  Fl_Button upButton{8, 40, 64, 30, "Up"};
  Fl_Input path{82, 40, 560, 30};
  Fl_Choice tabChoice{650, 40, 150, 30}, layoutChoice{810, 40, 180, 30};
  Fl_Tile panels{8, 80, 984, 580};
  Sidebar sidebar;
  Fl_Hold_Browser &list = sidebar.list;
  Fl_Multiline_Output &inspector = sidebar.inspector;
  SpatialView spatial{258, 80, 734, 580};
  Fl_Box status{8, 665, 984, 30};
  struct Tab {
    fsv::Location location;
    ViewState view;
    std::vector<fsv::Location> history;
    size_t cursor = 0;
  };
  std::vector<Tab> tabs;
  int tab = 0;
  fsv::Location location;
  fsv::Scene scene;
  std::vector<int> listed;
  int selected = -1;
  int sidebarWidth = 250;
  std::string layout = "map", filter;
  fsv::SortOptions sorting;
  std::unique_ptr<DesktopMenu> desktop;
  std::unique_ptr<ModelPreview> modelPreview;
  struct ScanResult {
    fsv::Result result;
    fsv::Location location;
  };
  std::future<ScanResult> scanning;
  std::shared_ptr<std::atomic_bool> cancelled;
  std::optional<fsv::Location> pending;
  std::future<std::string> operation;
  std::vector<std::string> clipboard;
  bool cut = false;
  fs::path temporary;
  uint64_t preview = 0;
  DevicesClient devicesClient;
  Json deviceRows = Json::array();
  std::string deviceSignature, deviceError, pendingDevice;
  bool deviceReady = false, automountEnabled = true, openAfterMount = false;
  std::chrono::steady_clock::time_point nextDevices{}, pendingDeviceDeadline{};
  Json selectedDevice() const {
    int row = sidebar.devices.value() - 1;
    return row >= 0 && row < int(deviceRows.size()) ? deviceRows[row] : Json();
  }
  static std::string plainLabel(std::string label) {
    for (auto &c : label)
      if (static_cast<unsigned char>(c) < 32)
        c = ' ';
    return label;
  }
  void updateDevices(const Json &state) {
    deviceReady = true;
    automountEnabled = state.value("enabled", true);
    deviceError = state.value("error", std::string());
    Json rows = Json::array(
        {Json{{"id", "home"},
              {"name", "Home"},
              {"mounts", Json::array({getenv("HOME") ? getenv("HOME") : "/"})}},
         Json{{"id", "root"},
              {"name", "File System"},
              {"mounts", Json::array({"/"})}}});
    for (auto row : state.at("devices"))
      rows.push_back(std::move(row));
    auto signature = rows.dump();
    if (signature != deviceSignature) {
      auto previous = selectedDevice();
      std::string identity =
          previous.is_null() ? "" : previous.value("id", std::string());
      // Leave a disconnected/unmounted volume instead of
      // showing stale contents.
      for (const auto &old : deviceRows) {
        if (old.value("id", std::string()) == "root" ||
            old.value("id", std::string()) == "home")
          continue;
        for (const auto &mount : old.value("mounts", Json::array())) {
          std::string prefix = mount.get<std::string>();
          bool present = false;
          for (const auto &row : rows)
            for (const auto &m : row.value("mounts", Json::array()))
              if (m == mount)
                present = true;
          auto current = location.path.string();
          if (!present &&
              (current == prefix || current.rfind(prefix + "/", 0) == 0))
            load({getenv("HOME") ? getenv("HOME") : "/"}, false);
        }
      }
      deviceRows = std::move(rows);
      deviceSignature = signature;
      sidebar.devices.clear();
      int index = 0;
      for (const auto &row : deviceRows) {
        std::string label =
            plainLabel(row.value("name", std::string("Volume")));
        if (row.value("locked", false))
          label += " [locked]";
        else if (row.value("mounts", Json::array()).empty())
          label += " [unmounted]";
        if (row.value("busy", false))
          label += " [...]";
        if (!row.value("error", std::string()).empty())
          label += " [error]";
        sidebar.devices.add(label.c_str());
        if (row.value("id", std::string()) == identity)
          sidebar.devices.value(index + 1);
        ++index;
      }
    }
    if (!pendingDevice.empty()) {
      for (const auto &row : deviceRows)
        if (row.value("backing", std::string()) == pendingDevice) {
          auto id = row.value("id", std::string());
          pendingDevice.clear();
          for (size_t i = 0; i < deviceRows.size(); ++i)
            if (deviceRows[i].value("id", std::string()) == id)
              sidebar.devices.value(i + 1);
          deviceCommand("Open device");
          break;
        }
      if (std::chrono::steady_clock::now() > pendingDeviceDeadline) {
        pendingDevice.clear();
        status.copy_label("Unlocked device is not available yet. "
                          "Select it to open.");
      }
    }
  }
  void pollDevices() {
    try {
      if (auto result = devicesClient.result())
        updateDevices(Json::parse(*result));
    } catch (const std::exception &e) {
      deviceReady = false;
      deviceError = e.what();
    }
    try {
      if (auto result = devicesClient.result(true)) {
        if (openAfterMount && !result->empty()) {
          auto target = Json::parse(*result).value("path", std::string());
          if (!target.empty())
            load({target});
        }
        openAfterMount = false;
        status.copy_label("Device operation completed.");
        nextDevices = {};
      }
    } catch (const std::exception &e) {
      openAfterMount = false;
      pendingDevice.clear();
      status.copy_label(e.what());
      nextDevices = {};
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= nextDevices) {
      devicesClient.refresh();
      nextDevices = now + std::chrono::seconds(1);
    }
    auto row = selectedDevice();
    bool selected = !row.is_null(),
         idle = !devicesClient.busy() && !operation.valid();
    bool mounted = selected && !row.value("mounts", Json::array()).empty();
    bool removable = selected && row.value("removable", false);
    auto enable = [&](Fl_Widget &widget, bool value) {
      if (value)
        widget.activate();
      else
        widget.deactivate();
    };
    enable(sidebar.deviceOpen, selected && idle);
    enable(sidebar.deviceUnmount, removable && mounted && idle);
    enable(sidebar.deviceRemove,
           removable && idle &&
               (row.value("poweroff", false) || row.value("ejectable", false)));
    dx::enable_menu(menu, "Devices/Open device", selected && idle);
    dx::enable_menu(menu, "Devices/Unmount device",
                    removable && mounted && idle);
    dx::enable_menu(menu, "Devices/Safely remove drive",
                    sidebar.deviceRemove.active());
    dx::enable_menu(menu, "Devices/Lock encrypted device",
                    removable && idle &&
                        !row.value("backing", std::string()).empty());
    dx::enable_menu(menu, "Devices/Automount removable media",
                    deviceReady && idle);
    if (auto item = menu.find_item("Devices/Automount removable media")) {
      int index = int(item - menu.menu());
      int mode = menu.mode(index);
      if (automountEnabled)
        mode |= FL_MENU_VALUE;
      else
        mode &= ~FL_MENU_VALUE;
      menu.mode(index, mode);
    }
    sidebar.devicesTitle.copy_label(deviceReady && deviceError.empty()
                                        ? "Devices"
                                        : "Devices (service unavailable)");
    sidebar.devicesTitle.copy_tooltip(
        deviceError.empty() ? "Removable devices and mounted volumes"
                            : deviceError.c_str());
  }
  void deviceInfo() {
    auto row = selectedDevice();
    if (row.is_null())
      return;
    std::string text = plainLabel(row.value("name", std::string()));
    text += "\n" + row.value("device", std::string());
    for (const auto &mount : row.value("mounts", Json::array()))
      text += "\n" + mount.get<std::string>();
    if (row.value("locked", false))
      text += "\nEncrypted volume (locked)";
    if (!row.value("error", std::string()).empty())
      text += "\n" + row.value("error", std::string());
    inspector.value(text.c_str());
  }
  void deviceCommand(const std::string &name) {
    if (devicesClient.busy() || operation.valid()) {
      status.copy_label("Wait for the current operation.");
      return;
    }
    if (name == "Automount removable media") {
      devicesClient.automount(!automountEnabled);
      return;
    }
    auto row = selectedDevice();
    if (row.is_null())
      return;
    std::string identity = row.value("id", std::string());
    auto mounts = row.value("mounts", Json::array());
    if (name == "Open device") {
      if (!mounts.empty()) {
        load({mounts[0].get<std::string>()});
        return;
      }
      if (row.value("locked", false)) {
        auto secret =
            fl_password("Unlock %s:", "",
                        plainLabel(row.value("name", std::string())).c_str());
        if (!secret)
          return;
        devicesClient.act("unlock", identity, secret);
        pendingDevice = identity;
        pendingDeviceDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(130);
      } else {
        openAfterMount = true;
        devicesClient.act("mount", identity);
      }
    } else if (name == "Unmount device")
      devicesClient.act("unmount", identity);
    else if (name == "Lock encrypted device")
      devicesClient.act("lock", identity);
    else if (name == "Safely remove drive") {
      if (fl_choice("Unmount all volumes on %s and safely remove "
                    "the drive?",
                    "Cancel", "Remove", nullptr,
                    plainLabel(row.value("name", std::string())).c_str()) != 1)
        return;
      devicesClient.act("remove", identity);
    }
    status.copy_label("Device operation in progress...");
  }
  static void tick(void *p) {
    ((Browser *)p)->poll();
    auto s = static_cast<Browser *>(p);
    auto mode = s->domeMonitor.snapshot();
    if ((mode.known && mode.xr && mode.stereoSupported) || s->stereoPacked)
      s->redraw();
    Fl::repeat_timeout(.05, tick, p);
  }
  static void action(Fl_Widget *w, void *p) {
    auto s = (Browser *)p;
    auto bar = dynamic_cast<Fl_Menu_ *>(w);
    if (bar && bar->mvalue())
      s->command(bar->mvalue()->label());
  }
  void poll() {
    pollDevices();
    bool available = entry() != nullptr, idle = !operation.valid(),
         writable = !location.archive && idle;
    bool model = entry() && !entry()->directory && entry()->member.empty() &&
                 fsv::gltf::supported(entry()->path);
    dx::enable_menu(menu, "File/Live glTF preview", model);
    dx::enable_menu(menu, "File/Open in XR",
                    model && modelPreview && modelPreview->canOpenXR());
    for (auto name : {"File/Open", "File/Properties", "Edit/Copy location"})
      dx::enable_menu(menu, name, available);
    for (auto name : {"File/Rename...", "File/Move to Trash",
                      "File/Compress...", "Edit/Cut", "Edit/Copy"})
      dx::enable_menu(menu, name, available && writable);
    dx::enable_menu(menu, "File/New folder...", writable);
    dx::enable_menu(menu, "Edit/Paste", writable && !clipboard.empty());
    dx::enable_menu(menu, "Go/Back", !tabs.empty() && tabs[tab].cursor > 0);
    dx::enable_menu(menu, "Go/Forward",
                    !tabs.empty() &&
                        tabs[tab].cursor + 1 < tabs[tab].history.size());

    if (scanning.valid() && scanning.wait_for(std::chrono::seconds(0)) ==
                                std::future_status::ready) {
      try {
        auto result = scanning.get();
        if (!*cancelled && !pending) {
          location = result.location;
          tabs[tab].location = location;
          if (!tabs[tab].history.empty())
            tabs[tab].history[tabs[tab].cursor] = location;
          path.value(location.display().c_str());
          scene = fsv::arrange(
              fsv::sortEntries(std::move(result.result.entries), sorting),
              layout);
          spatial.setScene(&scene);
          fillList();
          status.copy_label((std::to_string(scene.entries.size()) +
                             " entries. " + result.result.warning +
                             "  Wheel: zoom · Right drag: "
                             "orbit · Middle/Shift "
                             "drag: pan · F: focus · R: reset")
                                .c_str());
        }
      } catch (const std::exception &e) {
        status.copy_label(e.what());
      }
      if (pending) {
        auto next = *pending;
        pending.reset();
        startScan(next);
      }
    }
    if (operation.valid() && operation.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
      auto error = operation.get();
      if (!error.empty())
        fl_alert("%s", error.c_str());
      load(location, false);
    }
  }
  void startScan(fsv::Location target) {
    cancelled = std::make_shared<std::atomic_bool>(false);
    auto token = cancelled;
    std::promise<ScanResult> promise;
    scanning = promise.get_future();
    std::thread([target, token, p = std::move(promise)]() mutable {
      try {
        if (!target.archive) {
          target.path = fs::canonical(target.path);
          if (!fs::is_directory(target.path)) {
            if (!fsv::archiveSupported(target.path))
              throw std::runtime_error("Not a "
                                       "directory or "
                                       "supported "
                                       "archive");
            target.archive = true;
          }
        }
        p.set_value({fsv::scan(target, token), target});
      } catch (...) {
        p.set_exception(std::current_exception());
      }
    }).detach();
    status.copy_label("Scanning filesystem...");
  }
  void fillList() {
    list.clear();
    listed.clear();
    for (int i = 0; i < int(scene.entries.size()); ++i) {
      auto &e = scene.entries[i];
      if (e.parent != -1)
        continue;
      if (!filter.empty() && e.name.find(filter) == std::string::npos)
        continue;
      listed.push_back(i);
      auto name = (e.directory ? "[folder] " : "         ") + e.name;
      list.add(name.c_str());
    }
    select(-1);
    traceSort();
  }
  void select(int value) {
    selected = value;
    spatial.setSelected(value);
    int row = 0;
    for (int i = 0; i < int(listed.size()); ++i)
      if (listed[i] == value)
        row = i + 1;
    list.value(row);
    if (value < 0 || value >= int(scene.entries.size())) {
      inspector.value("Select a file or platform.\nMapV, TreeV and "
                      "DiscV\nrendered directly with OpenGL.");
      return;
    }
    auto &e = scene.entries[value];
    std::ostringstream out;
    out << e.name << "\n"
        << (e.directory ? "Directory"
            : e.symlink ? "Symbolic link"
                        : "File")
        << " · " << e.bytes << " bytes\n"
        << (e.member.empty() ? e.path.string()
                             : e.path.string() + "!/" + e.member);
    out << "\nType: " << e.type << "\nModified: " << date(e.modified)
        << "\nCreated: " << date(e.created)
        << "\nAccessed: " << date(e.accessed);
    if (!e.directory &&
        fsv::gltf::supported(e.member.empty() ? e.path : fs::path(e.member))) {
      if (e.member.empty()) {
        out << "\nDouble-click: live 3D preview. File "
               "→ Open in XR for controller "
               "manipulation.";
        if (!modelPreview)
          modelPreview = std::make_unique<ModelPreview>();
        modelPreview->open(e.path, false);
      } else
        out << "\nExtract the model and resources for "
               "a live glTF preview.";
    }
    inspector.value(out.str().c_str());
  }
  const fsv::Entry *entry() const {
    return selected >= 0 && selected < int(scene.entries.size())
               ? &scene.entries[selected]
               : nullptr;
  }
  static std::string date(const std::optional<int64_t> &value) {
    if (!value)
      return "Unavailable";
    std::time_t seconds = *value / 1000000000;
    std::tm result{};
    if (!localtime_r(&seconds, &result))
      return "Unavailable";
    std::ostringstream text;
    text << std::put_time(&result, "%Y-%m-%d %H:%M:%S");
    return text.str();
  }
  static fs::path settingsPath() {
    auto base = getenv("XDG_CONFIG_HOME");
    auto home = getenv("HOME");
    return fs::path(base   ? base
                    : home ? std::string(home) + "/.config"
                           : "/tmp") /
           "fsvr/fltk.json";
  }
  void settings(bool save) {
    try {
      auto path = settingsPath();
      if (save) {
        fs::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary);
        file << Json{{"sort", fsv::sortKeyName(sorting.key)},
                     {"descending", sorting.descending},
                     {"folders_first", sorting.foldersFirst},
                     {"case_sensitive", sorting.caseSensitive},
                     {"stereo_3d", stereoEnabled}}
                    .dump(2);
        file.close();
        if (!file)
          return;
        fs::rename(temporary, path);
      } else {
        if (!fs::exists(path) || fs::file_size(path) > 4096)
          return;
        Json value;
        std::ifstream(path) >> value;
        fsv::SortOptions candidate;
        if (auto key = fsv::sortKey(value.value("sort", std::string("name"))))
          candidate.key = *key;
        candidate.descending = value.value("descending", false);
        candidate.foldersFirst = value.value("folders_first", true);
        candidate.caseSensitive = value.value("case_sensitive", false);
        sorting = candidate;
        stereoEnabled = value.value("stereo_3d", true);
      }
    } catch (const std::exception &) {
    }
  }
  static const char *sortLabel(fsv::SortKey key) {
    switch (key) {
    case fsv::SortKey::Name:
      return "Name";
    case fsv::SortKey::Size:
      return "Size";
    case fsv::SortKey::Modified:
      return "Date modified";
    case fsv::SortKey::Type:
      return "Type";
    case fsv::SortKey::Created:
      return "Date created";
    case fsv::SortKey::Accessed:
      return "Date accessed";
    case fsv::SortKey::Extension:
      return "Extension";
    }
    return "Name";
  }
  void sortMenu(Fl_Menu_ &bar, const std::string &prefix) {
    for (auto key :
         {fsv::SortKey::Name, fsv::SortKey::Size, fsv::SortKey::Modified,
          fsv::SortKey::Type, fsv::SortKey::Created, fsv::SortKey::Accessed,
          fsv::SortKey::Extension})
      bar.add((prefix + sortLabel(key)).c_str(), 0, action, this,
              FL_MENU_RADIO | (sorting.key == key ? FL_MENU_VALUE : 0) |
                  (key == fsv::SortKey::Extension ? FL_MENU_DIVIDER : 0));
    bar.add((prefix + "Ascending").c_str(), 0, action, this,
            FL_MENU_RADIO | (!sorting.descending ? FL_MENU_VALUE : 0));
    bar.add((prefix + "Descending").c_str(), 0, action, this,
            FL_MENU_RADIO | FL_MENU_DIVIDER |
                (sorting.descending ? FL_MENU_VALUE : 0));
    bar.add((prefix + "Folders first").c_str(), 0, action, this,
            FL_MENU_TOGGLE | (sorting.foldersFirst ? FL_MENU_VALUE : 0));
    bar.add((prefix + "Case sensitive").c_str(), 0, action, this,
            FL_MENU_TOGGLE | (sorting.caseSensitive ? FL_MENU_VALUE : 0));
  }
  void syncSortMenu() {
    for (int i = 0; i < menu.size(); ++i) {
      auto label = menu.text(i);
      if (!label)
        continue;
      std::optional<bool> checked;
      for (auto key :
           {fsv::SortKey::Name, fsv::SortKey::Size, fsv::SortKey::Modified,
            fsv::SortKey::Type, fsv::SortKey::Created, fsv::SortKey::Accessed,
            fsv::SortKey::Extension})
        if (std::string(label) == sortLabel(key))
          checked = sorting.key == key;
      if (std::string(label) == "Ascending")
        checked = !sorting.descending;
      if (std::string(label) == "Descending")
        checked = sorting.descending;
      if (std::string(label) == "Folders first")
        checked = sorting.foldersFirst;
      if (std::string(label) == "Case sensitive")
        checked = sorting.caseSensitive;
      if (checked)
        menu.mode(i, (*checked ? menu.mode(i) | FL_MENU_VALUE
                               : menu.mode(i) & ~FL_MENU_VALUE));
    }
  }
  void traceSort() {
    if (!getenv("FSVR_SORT_TRACE"))
      return;
    Json names = Json::array();
    for (int i : listed)
      names.push_back(scene.entries[i].name);
    fprintf(stderr, "FSVR_SORT %s\n",
            Json{{"sort", fsv::sortKeyName(sorting.key)},
                 {"descending", sorting.descending},
                 {"folders_first", sorting.foldersFirst},
                 {"case_sensitive", sorting.caseSensitive},
                     {"stereo_3d", stereoEnabled},
                 {"selected", entry() ? entry()->name : std::string()},
                 {"entries", names}}
                .dump()
                .c_str());
  }
  bool sortCommand(const std::string &name) {
    bool changed = false;
    for (auto key :
         {fsv::SortKey::Name, fsv::SortKey::Size, fsv::SortKey::Modified,
          fsv::SortKey::Type, fsv::SortKey::Created, fsv::SortKey::Accessed,
          fsv::SortKey::Extension})
      if (name == sortLabel(key)) {
        if (sorting.key != key)
          sorting.descending =
              key == fsv::SortKey::Size || key == fsv::SortKey::Modified ||
              key == fsv::SortKey::Created || key == fsv::SortKey::Accessed;
        sorting.key = key;
        changed = true;
      }
    if (name == "Ascending") {
      sorting.descending = false;
      changed = true;
    }
    if (name == "Descending") {
      sorting.descending = true;
      changed = true;
    }
    if (name == "Folders first") {
      sorting.foldersFirst = !sorting.foldersFirst;
      changed = true;
    }
    if (name == "Case sensitive") {
      sorting.caseSensitive = !sorting.caseSensitive;
      changed = true;
    }
    if (!changed)
      return false;
    fs::path selectedPath;
    std::string selectedMember;
    if (auto e = entry()) {
      selectedPath = e->path;
      selectedMember = e->member;
    }
    scene = fsv::arrange(fsv::sortEntries(std::move(scene.entries), sorting),
                         layout);
    spatial.setScene(&scene);
    fillList();
    for (int i = 0; i < int(scene.entries.size()); ++i)
      if (scene.entries[i].path == selectedPath &&
          scene.entries[i].member == selectedMember) {
        select(i);
        break;
      }
    syncSortMenu();
    settings(true);
    traceSort();
    redraw();
    return true;
  }
  void updateTabs() {
    tabChoice.clear();
    for (auto &t : tabs) {
      auto name = t.location.archive ? t.location.path.filename().string() + "!"
                                     : t.location.path.filename().string();
      if (name.empty())
        name = "/";
      tabChoice.add(name.c_str());
    }
    tabChoice.value(tab);
  }
  template <class F> void work(F fn) {
    if (operation.valid()) {
      status.copy_label("Wait for the current file operation.");
      return;
    }
    operation = std::async(std::launch::async, [fn]() {
      try {
        fn();
        return std::string();
      } catch (const std::exception &e) {
        return std::string(e.what());
      }
    });
    status.copy_label("File operation in progress...");
  }
  void newTab(fsv::Location target) {
    if (!tabs.empty())
      tabs[tab].view = spatial.view;
    tabs.push_back({});
    tab = tabs.size() - 1;
    spatial.reset();
    load(target);
  }
  void activateTab(int value) {
    if (value < 0 || value >= int(tabs.size()) || value == tab)
      return;
    tabs[tab].view = spatial.view;
    tab = value;
    spatial.view = tabs[tab].view;
    load(tabs[tab].location, false);
  }
  void context() {
    Fl_Menu_Button popup(Fl::event_x(), Fl::event_y(), 0, 0);
    popup.type(Fl_Menu_Button::POPUP3);
    for (auto label :
         {"Open", "Open in new tab", "Live glTF preview", "Open in XR",
          "Properties", "Cut", "Copy", "Paste", "Rename...", "Move to Trash",
          "Compress...", "Extract archive"})
      popup.add(label, 0, action, this);
    auto e = entry();
    bool model = e && !e->directory && e->member.empty() &&
                 fsv::gltf::supported(e->path);
    dx::enable_menu(popup, "Live glTF preview", model);
    dx::enable_menu(popup, "Open in XR",
                    model && modelPreview && modelPreview->canOpenXR());
    sortMenu(popup, "Sort by/");
    dx::style(&popup);
    popup.popup();
  }

public:
  Browser() : dx::GpuWindow(1000, 700, "fsvr — FLTK OpenGL") {
    mode(FL_RGB | FL_ALPHA | FL_DOUBLE | FL_DEPTH);
    xclass("org.fsvxr.fsvr.Sbs");
    panels.end();
    end();
    add(status);
    sidebar.resize(sidebar.x(), sidebar.y(), sidebar.w(), sidebar.h());
    sidebar.devices.callback(
        [](Fl_Widget *, void *p) {
          static_cast<Browser *>(p)->deviceInfo();
          if (Fl::event_clicks())
            static_cast<Browser *>(p)->deviceCommand("Open device");
        },
        this);
    sidebar.deviceOpen.callback(
        [](Fl_Widget *, void *p) {
          static_cast<Browser *>(p)->deviceCommand("Open device");
        },
        this);
    sidebar.deviceUnmount.callback(
        [](Fl_Widget *, void *p) {
          static_cast<Browser *>(p)->deviceCommand("Unmount device");
        },
        this);
    sidebar.deviceRemove.callback(
        [](Fl_Widget *, void *p) {
          static_cast<Browser *>(p)->deviceCommand("Safely remove drive");
        },
        this);
    list.format_char(0);
    list.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Browser *)p;
          int i = s->list.value() - 1;
          if (i >= 0 && i < int(s->listed.size()))
            s->select(s->listed[i]);
          if (Fl::event_clicks())
            s->open();
        },
        this);
    for (auto item : std::vector<std::pair<const char *, int>>{
             {"File/Open", FL_Enter},
             {"File/Live glTF preview", 0},
             {"File/Open in XR", 0},
             {"File/Open in new tab", FL_CTRL + FL_Enter},
             {"File/New tab", FL_CTRL + 't'},
             {"File/Close tab", FL_CTRL + 'w'},
             {"File/New folder...", FL_CTRL + FL_SHIFT + 'n'},
             {"File/Rename...", FL_F + 2},
             {"File/Properties", FL_ALT + FL_Enter},
             {"File/Compress...", 0},
             {"File/Extract archive", 0},
             {"File/Move to Trash", FL_Delete},
             {"File/Quit", FL_CTRL + 'q'},
             {"Edit/Cut", FL_CTRL + 'x'},
             {"Edit/Copy", FL_CTRL + 'c'},
             {"Edit/Paste", FL_CTRL + 'v'},
             {"Edit/Copy location", FL_CTRL + FL_SHIFT + 'c'},
             {"Edit/Find...", FL_CTRL + 'f'},
             {"Go/Back", FL_ALT + FL_Left},
             {"Go/Forward", FL_ALT + FL_Right},
             {"Go/Up", FL_ALT + FL_Up},
             {"Go/Home", FL_ALT + FL_Home},
             {"Go/Refresh", FL_F + 5},
             {"View/Reset view", 'r'},
             {"View/Focus selection", 'f'},
             {"View/MapV", '1'},
             {"View/TreeV", '2'},
             {"View/DiscV", '3'},
             {"View/Spatial view", FL_F + 6},
             {"Tools/Terminal here", 0}})
      menu.add(item.first, item.second, action, this);
    for (auto item :
         {"Devices/Open device", "Devices/Unmount device",
          "Devices/Safely remove drive", "Devices/Lock encrypted device"})
      menu.add(item, 0, action, this);
    menu.add("Devices/Automount removable media", 0, action, this,
             FL_MENU_TOGGLE | FL_MENU_VALUE);
    menu.add("View/Stereo 3D", 0, action, this, FL_MENU_TOGGLE | FL_MENU_VALUE);
    settings(false);
    if (!stereoEnabled) menu.mode(menu.find_item("View/Stereo 3D") - menu.menu(), FL_MENU_TOGGLE);
    sortMenu(menu, "View/Sort by/");
    layoutChoice.add("MapV|TreeV|DiscV");
    layoutChoice.value(0);
    layoutChoice.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Browser *)p;
          s->setLayout(s->layoutChoice.value() == 0   ? "map"
                       : s->layoutChoice.value() == 1 ? "tree"
                                                      : "disc");
        },
        this);
    tabChoice.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Browser *)p;
          s->activateTab(s->tabChoice.value());
        },
        this);
    path.when(FL_WHEN_ENTER_KEY_ALWAYS);
    path.callback(
        [](Fl_Widget *, void *p) {
          auto s = (Browser *)p;
          try {
            s->load({s->path.value()});
          } catch (const std::exception &e) {
            s->status.copy_label(e.what());
          }
        },
        this);
    upButton.callback(
        [](Fl_Widget *, void *p) { ((Browser *)p)->command("Up"); }, this);
    spatial.selectionChanged = [this](int i) { select(i); };
    spatial.openRequested = [this](int i) {
      select(i);
      open();
    };
    spatial.contextRequested = [this](int i) {
      select(i);
      context();
    };
    status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    inspector.wrap(true);
    resizable(panels);
    panels.resizable(spatial);
    panels.init_size_range(160, 160);
    if (dx::dome()) {
      // Keep the real command tree for DBusMenu and keyboard
      // shortcuts.
      menu.hide();
      for (Fl_Widget *widget : std::vector<Fl_Widget *>{
               &upButton, &path, &tabChoice, &layoutChoice})
        widget->position(widget->x(), widget->y() - menu.h());
      panels.resize(panels.x(), panels.y() - menu.h(), panels.w(),
                    panels.h() + menu.h());
    }
    size_range(620, 400);
    callback(
        [](Fl_Widget *, void *p) { static_cast<Browser *>(p)->requestClose(); },
        this);
    char name[] = "/tmp/fsvr-fltk-XXXXXX";
    if (auto value = mkdtemp(name))
      temporary = value;
    desktop = std::make_unique<DesktopMenu>(*this, menu);
    Fl::add_timeout(.05, tick, this);
  }
  void draw() override {
    if (!context_valid()) stereo.abandon();
    damage(FL_DAMAGE_ALL);
    dx::GpuWindow::draw();
    stereo.mode = domeMonitor.snapshot();
    spatial.stereoPicking = false;
    stereoPacked = false;
    if (stereo.mode.known && stereo.mode.xr && stereo.mode.stereoSupported) {
      stereoPacked = true;
      stereo.X = spatial.x(); stereo.Y = spatial.y();
      stereo.W = spatial.w(); stereo.H = spatial.h();
      stereo.picking = [this](const fsv::gltf::Mat4 &vp,
                              std::function<fsv::gltf::Vec3(float,float)> surface) {
        spatial.stereoProjection = vp;
        spatial.stereoSurface = std::move(surface); spatial.stereoPicking = true;
      };
      stereo.render = [this](const fsv::gltf::Mat4 &vp, float width, float height,
                             float distance, int pw, int ph) {
        spatial.renderEye(vp, width, height, distance, pw, ph);
      };
      // Even with depth disabled, pack identical UI/scene images for both eyes.
      stereo.composite(w(), h(), pixel_w(), pixel_h(), stereoEnabled && spatial.visible());
    }
  }
  ~Browser() {
    if (Fl_Gl_Window::context()) { make_current(); stereo.release(); }
    Fl::remove_timeout(tick, this);
    if (cancelled)
      *cancelled = true;
    // Promise-backed scanner workers own their state and honour
    // cancellation; closing a browser must not wait for a slow
    // mount's metadata request.
    modelPreview.reset();
    if (operation.valid())
      operation.wait();
    if (!temporary.empty()) {
      std::error_code ec;
      fs::remove_all(temporary, ec);
    }
  }
  void requestClose() {
    if (operation.valid()) {
      status.copy_label("File operation in progress; the browser can close "
                        "when it finishes.");
      return;
    }
    if (modelPreview)
      modelPreview->hide();
    hide();
  }
  int handle(int event) override {
    if (event == FL_SHORTCUT && !menu.visible() && !Fl::modal() &&
        menu.handle(event))
      return 1;
    return dx::GpuWindow::handle(event);
  }
  void load(fsv::Location target, bool history = true) {
    try {
      if (operation.valid())
        throw std::runtime_error("Wait for the current file operation");
      if (!target.archive)
        target.path = fs::absolute(target.path).lexically_normal();
      if (tabs.empty()) {
        tabs.push_back({});
        tab = 0;
      }
      if (history) {
        auto &t = tabs[tab];
        if (t.history.empty() || !(t.history[t.cursor] == target)) {
          if (!t.history.empty())
            t.history.resize(t.cursor + 1);
          t.history.push_back(target);
          t.cursor = t.history.size() - 1;
        }
      }
      tabs[tab].location = target;
      location = target;
      path.value(target.display().c_str());
      updateTabs();
      copy_label(((target.archive
                       ? target.path.filename().string() + "!/" + target.member
                       : target.path.filename().string()) +
                  " — fsvr (FLTK)")
                     .c_str());
      scene = {};
      spatial.setScene(&scene);
      list.clear();
      listed.clear();
      select(-1);
      if (scanning.valid()) {
        *cancelled = true;
        pending = target;
      } else
        startScan(target);
    } catch (const std::exception &e) {
      status.copy_label(e.what());
    }
  }
  void open() {
    auto e = entry();
    if (!e)
      return;
    auto node = *e;
    if (node.directory) {
      load(node.member.empty()
               ? fsv::Location{node.path}
               : fsv::Location{location.path, node.member, true});
    } else if (node.member.empty() && fsv::gltf::supported(node.path)) {
      if (!modelPreview)
        modelPreview = std::make_unique<ModelPreview>();
      modelPreview->open(node.path, true);
    } else if (!node.member.empty()) {
      try {
        if (temporary.empty())
          throw std::runtime_error("Cannot create private archive "
                                   "preview");
        auto output = fsv::materialize({location.path, node.member, true},
                                       temporary / std::to_string(++preview));
        dx::spawn({"gio", "open", output.string()});
      } catch (const std::exception &error) {
        status.copy_label(error.what());
      }
    } else if (fsv::archiveSupported(node.path))
      load({node.path, "", true});
    else
      dx::spawn({"gio", "open", node.path.string()});
  }
  void setLayout(std::string value) {
    layout = value;
    layoutChoice.value(layout == "map" ? 0 : layout == "tree" ? 1 : 2);
    scene = fsv::arrange(std::move(scene.entries), layout);
    spatial.setScene(&scene);
    spatial.setSelected(selected);
  }
  void command(const std::string &name) {
    if (name == "Open device" || name == "Unmount device" ||
        name == "Safely remove drive" || name == "Lock encrypted device" ||
        name == "Automount removable media") {
      deviceCommand(name);
      return;
    }
    if (sortCommand(name))
      return;
    auto e = entry();
    if (name == "Open")
      open();
    else if (name == "Live glTF preview" || name == "Open in XR") {
      if (e && !e->directory && e->member.empty() &&
          fsv::gltf::supported(e->path)) {
        if (!modelPreview)
          modelPreview = std::make_unique<ModelPreview>();
        modelPreview->open(e->path, true, name == "Open in XR");
      }
    } else if (name == "Open in new tab") {
      if (e && e->directory)
        newTab(e->member.empty()
                   ? fsv::Location{e->path}
                   : fsv::Location{location.path, e->member, true});
    } else if (name == "New tab")
      newTab(location);
    else if (name == "Close tab") {
      if (tabs.size() == 1) {
        hide();
        return;
      }
      tabs.erase(tabs.begin() + tab);
      tab = std::min(tab, int(tabs.size()) - 1);
      spatial.view = tabs[tab].view;
      load(tabs[tab].location, false);
    } else if (name == "Back" || name == "Forward") {
      auto &t = tabs[tab];
      if (name == "Back" && t.cursor > 0)
        load(t.history[--t.cursor], false);
      else if (name == "Forward" && t.cursor + 1 < t.history.size())
        load(t.history[++t.cursor], false);
    } else if (name == "Up") {
      if (location.archive) {
        if (location.member.empty())
          load({location.path.parent_path()});
        else
          load({location.path,
                fs::path(location.member).parent_path().generic_string(),
                true});
      } else
        load({location.path.parent_path()});
    } else if (name == "Home")
      load({getenv("HOME") ? getenv("HOME") : "/"});
    else if (name == "Refresh")
      load(location, false);
    else if (name == "Reset view")
      spatial.reset();
    else if (name == "Focus selection")
      spatial.focusSelection();
    else if (name == "MapV")
      setLayout("map");
    else if (name == "TreeV")
      setLayout("tree");
    else if (name == "DiscV")
      setLayout("disc");
    else if (name == "Stereo 3D") {
      stereoEnabled = !stereoEnabled;
      menu.mode(menu.find_item("View/Stereo 3D") - menu.menu(),
                FL_MENU_TOGGLE | (stereoEnabled ? FL_MENU_VALUE : 0));
      settings(true);
      redraw();
    } else if (name == "Spatial view") {
      if (spatial.visible()) {
        sidebarWidth = sidebar.w();
        spatial.hide();
        // A tile considers even hidden children when
        // resizing and hit testing. Remove the hidden
        // viewport from the tile until it is restored.
        add(spatial);
        sidebar.resize(panels.x(), panels.y(), panels.w(), panels.h());
        panels.resizable(sidebar);
      } else {
        panels.add(spatial);
        int width = std::clamp(sidebarWidth, 160, panels.w() - 160);
        sidebar.resize(panels.x(), panels.y(), width, panels.h());
        spatial.resize(panels.x() + width, panels.y(), panels.w() - width,
                       panels.h());
        panels.resizable(spatial);
        spatial.show();
      }
      panels.init_sizes();
      init_sizes();
      redraw();
    } else if (name == "Properties") {
      if (e)
        fl_message("%s\n%s\n%s · %llu bytes\nModified: "
                   "%s\nCreated: %s\nAccessed: %s",
                   e->name.c_str(), e->path.c_str(), e->type.c_str(),
                   (unsigned long long)e->bytes, date(e->modified).c_str(),
                   date(e->created).c_str(), date(e->accessed).c_str());
    } else if (name == "Find...") {
      auto value =
          fl_input("Filter this directory (empty shows all):", filter.c_str());
      if (value) {
        filter = value;
        fillList();
      }
    } else if (name == "Copy location") {
      auto text =
          e ? (e->member.empty() ? e->path.string()
                                 : location.path.string() + "!/" + e->member)
            : location.display();
      Fl::copy(text.data(), text.size(), 1);
    } else if (name == "Terminal here") {
      if (!location.archive)
        dx::spawn({"env", "-C", location.path.string(), "x-terminal-emulator"});
    } else if (name == "Quit")
      requestClose();
    else if (name == "Compress...") {
      if (!e || location.archive)
        return;
      auto source = e->path;
      Fl_Native_File_Chooser chooser;
      chooser.title("Create archive");
      chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
      chooser.options(Fl_Native_File_Chooser::NEW_FOLDER);
      chooser.directory(source.parent_path().c_str());
      chooser.preset_file((source.filename().string() + ".zip").c_str());
      chooser.filter("Archives\t*.{zip,tar,tar.gz,tgz,tar.xz,tar.zst}");
      if (chooser.show() == 0) {
        fs::path destination = chooser.filename();
        work([source, destination] { fsv::compress(source, destination); });
      }
    } else if (name == "Extract archive") {
      auto source = location.archive ? location.path : e ? e->path : fs::path();
      if (!source.empty() && fsv::archiveSupported(source))
        work([source] { fsv::extract(source); });
    } else if (location.archive) {
      status.copy_label("Archive contents are read-only. Extract to modify "
                        "them.");
    } else if (name == "Cut" || name == "Copy") {
      if (e) {
        clipboard = {e->path.string()};
        cut = name == "Cut";
        std::string uri = "file://" + e->path.string() + "\r\n";
        Fl::copy(uri.data(), uri.size(), 1);
      }
    } else if (name == "Paste") {
      auto sources = clipboard;
      auto destination = location.path;
      bool move = cut;
      if (move)
        clipboard.clear();
      work([sources, destination, move] {
        for (auto &source : sources) {
          auto target = destination / fs::path(source).filename();
          if (fs::exists(target))
            throw std::runtime_error("Destination exists: " + target.string());
          if (move)
            fs::rename(source, target);
          else
            fs::copy(source, target,
                     fs::copy_options::recursive |
                         fs::copy_options::copy_symlinks);
        }
      });
    } else if (name == "New folder...") {
      auto value = fl_input("New folder name:");
      if (value) {
        std::string folder = value;
        auto parent = location.path;
        work([folder, parent] {
          if (folder.empty() || fs::path(folder).filename() != folder ||
              folder == "." || folder == "..")
            throw std::runtime_error("Enter a single folder "
                                     "name");
          if (!fs::create_directory(parent / folder))
            throw std::runtime_error("Folder already exists");
        });
      }
    } else if (name == "Rename...") {
      if (!e)
        return;
      auto source = e->path;
      auto value = fl_input("Rename to:", source.filename().c_str());
      if (value) {
        std::string name = value;
        work([source, name] {
          if (name.empty() || fs::path(name).filename() != name ||
              name == "." || name == "..")
            throw std::runtime_error("Enter a single filename");
          auto dest = source.parent_path() / name;
          if (fs::exists(dest))
            throw std::runtime_error("Destination exists");
          fs::rename(source, dest);
        });
      }
    } else if (name == "Move to Trash") {
      if (!e)
        return;
      auto source = e->path;
      if (fl_choice("Move selected item to Trash?", "Cancel", "Move",
                    nullptr) == 1) {
        work([source] {
          auto pid = dx::spawn({"gio", "trash", source.string()});
          int status = 0;
          if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
              WEXITSTATUS(status))
            throw std::runtime_error("Moving to Trash failed");
        });
      }
    }
  }
  Json stereoStatus() const {
    return {{"enabled", stereoEnabled}, {"xr", stereo.mode.known && stereo.mode.xr},
            {"supported", stereo.mode.stereoSupported}, {"curved", stereo.mode.hasCylinder},
            {"tracked", stereo.mode.trackedEyes}, {"frames", stereo.stereoFrames},
            {"error", stereo.error}, {"picking", spatial.stereoPicking}};
  }
  bool idle() const { return !scanning.valid() && !operation.valid(); }
  void smoke(const char *output) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!idle() && std::chrono::steady_clock::now() < deadline)
      Fl::wait(.02);
    spatial.flush();
    if (output) {
      spatial.snapshot(std::string(output) + ".scene.ppm");
      dx::screenshot(*this, output);
    }
  }
  fsv::Scene &currentScene() { return scene; }
};
int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--model-probe") {
    auto p = fsv::gltf::probeXr();
    puts(Json{{"available", p.available},
              {"runtime", p.runtime},
              {"message", p.message}}
             .dump()
             .c_str());
    return 0;
  }
  if (argc > 2 && (std::string(argv[1]) == "--model-xr" ||
                   std::string(argv[1]) == "--model-render")) {
    try {
      std::string mode = argv[1];
      double seconds = 0;
      fs::path report;
      for (int i = mode == "--model-render" ? 4 : 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--seconds" && i + 1 < argc)
          seconds = std::stod(argv[++i]);
        else if (std::string(argv[i]) == "--report" && i + 1 < argc)
          report = argv[++i];
        else
          throw std::runtime_error("Unknown model viewer argument");
      }
      if (mode == "--model-render") {
        if (argc < 4)
          throw std::runtime_error("--model-render requires FILE "
                                   "OUTPUT.ppm");
        fsv::gltf::renderModel(argv[2], argv[3], seconds);
        return 0;
      }
      auto parent = getppid();
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      if (parent == 1 || getppid() != parent)
        return 1;
      return fsv::gltf::runXr(argv[2], seconds, report);
    } catch (const std::exception &e) {
      fprintf(stderr, "fsvr: %s\n", e.what());
      return 1;
    }
  }
  if (argc > 1 && std::string(argv[1]) == "--version") {
    puts("fsvr FLTK 1.4.5 OpenGL");
    return 0;
  }
  dx::init();
  fl_register_images();
  Browser browser;
  fsv::Location location{getenv("HOME") ? getenv("HOME") : "/"};
  const char *smoke = nullptr;
  std::string layout = "map";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--smoke-test" && i + 1 < argc)
      smoke = argv[++i];
    else if (arg == "--layout" && i + 1 < argc)
      layout = argv[++i];
    else if (arg == "--openxr" || arg == "--xr") {
      fprintf(stderr, "fsvr: standalone OpenXR is not ported. Launch "
                      "fsvr through DomeD's "
                      "XR application launcher for the GPU "
                      "Wayland/XR frontend.\n");
      return 2;
    } else if (argv[i][0] != '-')
      location.path = argv[i];
  }
  browser.load(location);
  browser.setLayout(layout);
  dx::style(&browser);
  browser.show();
  if (smoke) {
    browser.smoke(smoke);
    printf("{\"toolkit\":\"FLTK\",\"renderer\":\"OpenGL\",\"entries\":"
           "%zu}\n",
           browser.currentScene().entries.size());
    return 0;
  }
  return Fl::run();
}
