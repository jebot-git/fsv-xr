// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the actual browser widgets and FLTK's subwindow event translation.
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <gio/gio.h>
#include <iostream>
#include <stdexcept>

template <class T> T *findWidget(Fl_Group &group) {
  for (int i = 0; i < group.children(); ++i) {
    auto child = group.child(i);
    if (auto match = dynamic_cast<T *>(child)) return match;
    if (auto nested = dynamic_cast<Fl_Group *>(child))
      if (auto match = findWidget<T>(*nested)) return match;
  }
  return nullptr;
}
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void settle() { for (int i = 0; i < 10; ++i) Fl::wait(.01); }
GVariant *menuCall(const char *method, GVariant *arguments) {
  auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  require(bus, "A private session bus is required");
  struct Result { bool done = false; GVariant *reply = nullptr; GError *error = nullptr; } result;
  g_dbus_connection_call(bus, g_dbus_connection_get_unique_name(bus),
      "/org/de_xr/FLTK/Menu1", "com.canonical.dbusmenu", method, arguments,
      nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
      [](GObject *object, GAsyncResult *async, gpointer data) {
        auto &result = *static_cast<Result *>(data);
        result.reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), async, &result.error);
        result.done = true;
      }, &result);
  while (!result.done) {
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    Fl::wait(.01);
  }
  g_object_unref(bus);
  if (result.error) {
    std::string message = result.error->message;
    g_error_free(result.error);
    throw std::runtime_error(message);
  }
  return result.reply;
}
int commandId(GVariant *node, const char *label) {
  auto props = g_variant_get_child_value(node, 1);
  const char *name = nullptr;
  bool match = g_variant_lookup(props, "label", "&s", &name) && !strcmp(name, label);
  g_variant_unref(props);
  if (match) {
    gint32 id;
    g_variant_get_child(node, 0, "i", &id);
    return id;
  }
  auto children = g_variant_get_child_value(node, 2);
  int id = -1;
  for (size_t i = 0; i < g_variant_n_children(children) && id < 0; ++i) {
    auto wrapped = g_variant_get_child_value(children, i);
    auto child = g_variant_get_variant(wrapped);
    id = commandId(child, label);
    g_variant_unref(child); g_variant_unref(wrapped);
  }
  g_variant_unref(children);
  return id;
}
void click(Browser &browser, int x, int y) {
  Fl::e_x = x; Fl::e_y = y; Fl::e_keysym = FL_Button + 1;
  Fl::e_state = FL_BUTTON1; Fl::e_clicks = 0; Fl::e_is_click = 1;
  Fl::handle(FL_PUSH, &browser);
  Fl::e_x = x; Fl::e_y = y; Fl::e_state = 0;
  Fl::handle(FL_RELEASE, &browser);
  settle();
}
void clickFolder(Browser &browser, SpatialView &view, int index) {
  view.make_current(); view.draw();
  double x,y;
  auto &entry = browser.currentScene().entries.at(index);
  require(view.projectPoint({entry.center.x, entry.center.y + entry.extent.y / 2,
                             entry.center.z}, x, y), "Folder projection failed");
  click(browser, view.x() + std::lround(x), view.y() + std::lround(y));
  require(view.selectedIndex() == index,
          "A spatial click must select the folder under the pointer");
}
void contextMenu(Browser &browser, SpatialView &view, bool activate) {
  struct Probe {
    bool activate, hover = false, release = false, submenu = false,
         returned = false, finished = false;
    int step = 0;
  } probe{activate};
  auto tick = +[](void *data) {
    auto &p = *static_cast<Probe *>(data);
    auto menu = Fl::grab();
    if (!menu) { p.finished = true; return; }
    Fl::e_x = 12; Fl::e_y = 12;
    Fl::e_x_root = menu->x() + 12; Fl::e_y_root = menu->y() + 12;
    Fl::e_state = 0;
    if (p.step == 0) {
      Fl::handle(FL_ENTER, menu);
      Fl::handle(FL_MOVE, menu);
    } else if (p.step == 1) {
      p.hover = true;
      // A delayed/synthetic release with no matching press must not pick the
      // item highlighted by pointer entry. Movement clears FLTK's click flag.
      Fl::e_keysym = FL_Button + 3; Fl::e_is_click = 0;
      Fl::handle(FL_RELEASE, menu);
    } else if (p.step == 2) {
      p.release = true;
      Fl::e_y = menu->h() - 12;
      Fl::e_y_root = menu->y() + Fl::e_y;
      Fl::handle(FL_MOVE, menu); // Sort by opens its child menu.
    } else if (p.step == 3) {
      for (auto child = Fl::first_window(); child; child = Fl::next_window(child)) {
        if (child == menu || !child->menu_window()) continue;
        p.submenu = true;
        Fl::e_x_root = child->x() + 12; Fl::e_y_root = child->y() + 12;
        Fl::handle(FL_MOVE, child);
        break;
      }
    } else if (p.step == 4) {
      Fl::handle(FL_MOVE, menu); // Returning closes only the child menu.
    } else {
      p.returned = true;
      if (p.activate) {
        Fl::e_keysym = FL_Button + 1; Fl::e_state = FL_BUTTON1;
        Fl::handle(FL_PUSH, menu);
        Fl::e_state = 0;
        Fl::handle(FL_RELEASE, menu);
      } else {
        Fl::e_keysym = FL_Escape;
        Fl::handle(FL_KEYDOWN, menu);
      }
      p.finished = true;
    }
    ++p.step;
  };
  // Separate event-loop turns let the menu process every injected event.
  Fl::add_timeout(.05, tick, &probe);
  Fl::add_timeout(.10, tick, &probe);
  Fl::add_timeout(.15, tick, &probe);
  Fl::add_timeout(.20, tick, &probe);
  Fl::add_timeout(.25, tick, &probe);
  Fl::add_timeout(.30, tick, &probe);
  view.make_current(); view.draw();
  auto &entry = browser.currentScene().entries.at(1);
  double x, y;
  require(view.projectPoint({entry.center.x, entry.center.y + entry.extent.y / 2,
                             entry.center.z}, x, y), "Context target projection");
  Fl::e_x = view.x() + std::lround(x);
  Fl::e_y = view.y() + std::lround(y);
  Fl::e_x_root = browser.x() + Fl::e_x;
  Fl::e_y_root = browser.y() + Fl::e_y;
  Fl::e_keysym = FL_Button + 3; Fl::e_state = FL_BUTTON3;
  Fl::e_clicks = 0; Fl::e_is_click = 1;
  Fl::handle(FL_PUSH, &browser);
  Fl::e_state = 0;
  auto dispatch = Fl::event_dispatch();
  Fl::handle(FL_RELEASE, &browser);
  Fl::remove_timeout(tick, &probe);
  require(probe.hover && probe.release,
          "Hover and an unmatched release must leave the context menu open");
  require(probe.submenu && probe.returned,
          "Returning from a submenu must preserve the parent context menu");
  require(probe.finished, "Click or Escape must finish context menu interaction");
  require(!Fl::grab(), "Click or Escape must dismiss the context menu");
  require(Fl::event_dispatch() == dispatch, "Context menu must restore event dispatch");
  browser.smoke(nullptr);
}
void rightGesture(SpatialView &view) {
  auto context = view.contextRequested;
  int requests = 0;
  view.contextRequested = [&](int) { ++requests; };
  Fl::e_keysym = FL_Button + 3;
  view.handle(FL_PUSH);
  view.handle(FL_RELEASE);
  view.handle(FL_MOVE);
  view.handle(FL_RELEASE);
  bool once = requests == 1;
  auto yaw = view.view.yaw;
  view.handle(FL_PUSH);
  Fl::e_x += 20;
  view.handle(FL_DRAG);
  view.handle(FL_RELEASE);
  bool drag = requests == 1 && view.view.yaw != yaw;
  // A release for a different button cannot complete the context gesture.
  view.handle(FL_PUSH);
  Fl::e_keysym = FL_Button + 1;
  view.handle(FL_RELEASE);
  view.contextRequested = context;
  require(once && requests == 1, "A right click must request its menu only once");
  require(drag, "Right drag must rotate without opening a menu");
}
int main(int argc, char **argv) try {
  char temporary[] = "/tmp/fsvr-interaction-XXXXXX";
  require(mkdtemp(temporary), "Temporary fixture");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{temporary};
  setenv("XDG_CONFIG_HOME", temporary, 1);
  fs::path files = fs::path(temporary) / "files";
  fs::create_directories(files / "alpha");
  fs::create_directories(files / "beta");
  dx::init();
  Browser browser;
  browser.load({files}); browser.show(); browser.smoke(nullptr); settle();
  auto view = findWidget<SpatialView>(browser);
  auto sidebar = findWidget<Sidebar>(browser);
  auto tile = findWidget<Fl_Tile>(browser);
  auto menu = findWidget<Fl_Menu_Bar>(browser);
  require(view && sidebar && tile && menu, "Browser widgets must exist");
  require(menu->visible() != dx::dome(), "Dome must use only the exported menu");
  require(tile->y() == (dx::dome() ? 50 : 80), "Hidden menu space must be reclaimed");
  require(menu->find_item("File/Open") && menu->find_item("View/Spatial view"),
          "The exported command tree must remain available");
  auto reply = menuCall("GetLayout", g_variant_new("(ii@as)", 0, -1,
                                               g_variant_new_strv(nullptr, 0)));
  auto exported = g_variant_get_child_value(reply, 1);
  int toggle = commandId(exported, "Spatial view");
  require(toggle > 0 && commandId(exported, "Open") > 0 && commandId(exported, "Automount removable media") > 0 && commandId(exported, "Unmount device") > 0,
          "Universal menu must expose the real browser commands");
  g_variant_unref(exported); g_variant_unref(reply);
  for (bool visible : {false, true}) {
    reply = menuCall("Event", g_variant_new("(isvu)", toggle, "clicked",
                                            g_variant_new_int32(0), 0u));
    g_variant_unref(reply);
    require(view->visible() == visible, "Universal menu must activate the actual command");
  }
  require(browser.currentScene().entries.size() == 2, "Both fixture folders must load");
  for (auto layout : {"map", "tree", "disc"}) {
    browser.setLayout(layout); settle();
    click(browser, sidebar->list.x() + 40, sidebar->list.y() + 12);
    require(Fl::focus() && sidebar->contains(Fl::focus()), "Sidebar must take focus");
    for (int i = 0; i < 2; ++i) {
      clickFolder(browser, *view, i);
      require(sidebar->list.value() == i + 1, "Spatial selection must update the list");
    }
  }
  browser.setLayout("map");
  // Dragging the divider, hiding the view, and resizing must never leave an
  // expanded list covering the viewport when the spatial view is restored.
  tile->move_intersection(sidebar->x() + sidebar->w(), tile->y(),
                          tile->x() + 310, tile->y());
  tile->init_sizes();
  for (auto size : {std::pair<int,int>{1250, 820}, {700, 460}, {1000, 700}}) {
    browser.command("Spatial view");
    require(!view->visible() && sidebar->w() == tile->w(), "List-only view must fill the tile");
    browser.resize(browser.x(), browser.y(), size.first, size.second); settle();
    browser.command("Spatial view"); settle();
    require(sidebar->x() + sidebar->w() == view->x() &&
            view->x() + view->w() == tile->x() + tile->w(),
            "Sidebar and spatial view must meet without overlap or gaps");
    require(sidebar->y() == view->y() && sidebar->h() == view->h(),
            "Both panels must fill the resized content height");
    click(browser, sidebar->list.x() + 40, sidebar->list.y() + 12);
    clickFolder(browser, *view, 1);
  }
  if (dx::dome()) {
    Fl::e_keysym = FL_F + 6; Fl::e_state = 0;
    require(browser.handle(FL_SHORTCUT) && !view->visible(),
            "Hidden menu must retain F6 shortcut");
    require(browser.handle(FL_SHORTCUT) && view->visible(), "F6 must restore the view");
  }
  contextMenu(browser, *view, false);
  require(std::string(findWidget<Fl_Input>(browser)->value()) == files.string(),
          "Hover and Escape must not open a folder");
  contextMenu(browser, *view, true);
  require(std::string(findWidget<Fl_Input>(browser)->value()) == (files / "beta").string(),
          "A fresh menu click must activate Open on the right-clicked folder");
  browser.load({files}); browser.smoke(nullptr);
  rightGesture(*view);
  view->reset(); settle();
  clickFolder(browser, *view, 1);
  if (argc > 1) browser.smoke(argv[1]);
  browser.command("Open"); browser.smoke(nullptr);
  require(std::string(findWidget<Fl_Input>(browser)->value()) == (files / "beta").string(),
          "Opening the spatial selection must navigate to the clicked folder");
  auto closeTab = menu->find_item("File/Close tab");
  auto closeWindow = menu->find_item("File/Close window");
  require(closeTab && closeTab->shortcut() == (FL_CTRL + 'w') &&
          closeWindow && closeWindow->shortcut() == (FL_META + 'w'),
          "Tab and window close must have distinct Control/Super chords");
  browser.command("New tab"); browser.load({files}); browser.smoke(nullptr);
  auto tabs = findWidget<Fl_Choice>(browser); // tab choice is the first choice
  require(tabs && tabs->size() == 3, "Second tab must open");
  Fl::e_keysym = 'w'; Fl::e_state = FL_CTRL;
  require(menu->handle(FL_SHORTCUT), "Close-tab shortcut dispatch"); browser.smoke(nullptr);
  require(browser.shown() && tabs->size() == 2, "Ctrl+W closes one tab and leaves the window open");
  browser.command("New tab"); browser.load({files}); browser.smoke(nullptr);
  Fl::e_keysym = 'w'; Fl::e_state = FL_META;
  require(menu->handle(FL_SHORTCUT), "Close-window shortcut dispatch");
  require(!browser.shown(), "Super+W closes the window with multiple tabs");
  // Destruction of a still-focused window must clear focus before destroying
  // the clipboard/event handlers used by the resulting FL_UNFOCUS events.
  browser.show(); view->take_focus(); settle();
  std::cout << "PASS: " << (dx::dome() ? "Dome universal menu" : "standalone menu")
            << ", context hover/release/click/Escape, right drag, spatial picking in all layouts, sidebar focus, splitter/toggle/resize and open\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n'; return 1;
}
