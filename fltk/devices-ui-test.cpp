// SPDX-License-Identifier: LGPL-2.1-or-later
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
#include <stdexcept>

template <class F> void until(F condition) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (!condition() && std::chrono::steady_clock::now() < end)
    Fl::wait(.02);
  if (!condition())
    throw std::runtime_error("Timed out waiting for devices UI");
}
template <class T> T *widget(Fl_Group &group) {
  for (int i = 0; i < group.children(); ++i) {
    if (auto result = dynamic_cast<T *>(group.child(i)))
      return result;
    if (auto child = dynamic_cast<Fl_Group *>(group.child(i)))
      if (auto result = widget<T>(*child))
        return result;
  }
  return nullptr;
}
int main() try {
  dx::init();
  Browser browser;
  browser.load({getenv("FSVR_TEST_MOUNT")});
  dx::style(&browser);
  browser.show();
  auto sidebar = widget<Sidebar>(browser);
  auto input = widget<Fl_Input>(browser);
  auto menu = widget<Fl_Menu_Bar>(browser);
  if (!sidebar || !input || !menu)
    throw std::runtime_error("Missing UI controls");
  until([&] { return sidebar->devices.size() == 3; });
  sidebar->devices.value(3);
  browser.command("Open device");
  until(
      [&] { return std::string(input->value()) == getenv("FSVR_TEST_MOUNT"); });
  browser.command("Unmount device");
  until([&] {
    return std::string(sidebar->devices.text(3)).find("unmounted") !=
           std::string::npos;
  });
  if (std::string(input->value()) == getenv("FSVR_TEST_MOUNT"))
    throw std::runtime_error("Unmount left stale view");
  sidebar->devices.value(3);
  browser.command("Open device");
  until([&] {
    return std::string(sidebar->devices.text(3)).find("unmounted") ==
               std::string::npos &&
           std::string(input->value()) == getenv("FSVR_TEST_MOUNT");
  });
  until([&] {
    return menu->find_item("Devices/Automount removable media")->active();
  });
  browser.command("Automount removable media");
  until([&] {
    return !menu->find_item("Devices/Automount removable media")->value();
  });
  browser.command("Automount removable media");
  until([&] {
    return menu->find_item("Devices/Automount removable media")->value();
  });
  browser.resize(browser.x(), browser.y(), 700, 460);
  Fl::wait(.1);
  if (sidebar->devices.y() < sidebar->list.y() + sidebar->list.h() ||
      sidebar->inspector.y() <
          sidebar->deviceOpen.y() + sidebar->deviceOpen.h())
    throw std::runtime_error("Device widgets overlap at minimum size");
  if (!menu->find_item("Devices/Safely remove drive") ||
      !menu->find_item("Devices/Lock encrypted device"))
    throw std::runtime_error("Missing device menu commands");
  dx::screenshot(browser, getenv("FSVR_DEVICE_UI_SCREENSHOT"));
  browser.hide();
  std::cout << "FLTK devices UI: live list, open, mount, unmount, view "
               "fallback, toggle, resize and commands passed\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
