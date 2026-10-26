// SPDX-License-Identifier: LGPL-2.1-or-later
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
#include <dlfcn.h>
#include <X11/Xlib.h>
void require(bool okay, const char *message) { if (!okay) throw std::runtime_error(message); }
int authentications = 0;
void settle(Browser &browser) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    Fl::wait(.02);
    for (auto window = Fl::first_window(); window; window = Fl::next_window(window))
      if (auto login = dynamic_cast<SmbLogin *>(window)) {
        for (int i = 0; i < login->children(); ++i) {
          auto child = login->child(i);
          std::string label = child->label() ? child->label() : "";
          if (auto field = dynamic_cast<Fl_Input *>(child)) {
            if (label == "Username:") field->value("root");
            if (label == "Domain:") field->value("WORKGROUP");
            if (label == "Password:") field->value(getenv("FSVR_SMB_TEST_PASSWORD"));
          }
        }
        for (int i = 0; i < login->children(); ++i)
          if (auto button = dynamic_cast<Fl_Return_Button *>(login->child(i))) {
            button->do_callback(); ++authentications; break;
          }
        break;
      }
    if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("Network UI timed out");
  } while (!browser.idle());
  for (int i = 0; i < 5; ++i) Fl::wait(.02);
}
int main() try {
  dx::init(); fl_register_images();
  auto uri = std::string(getenv("FSVR_SMB_TEST_URI"));
  fs::path server = getenv("FSVR_SMB_TEST_SOURCE");
  fs::path local = fs::path(getenv("HOME")) / "download"; fs::create_directory(local);
  Browser remote, destination;
  remote.load(fsv::locationArgument(uri)); remote.show(); settle(remote);
  require(!remote.currentScene().entries.empty(), "Share maps to native scene");
  remote.command("Add bookmark");
  bool saved = false;
  for (auto &place : fsv::places()) if (place.location.network == uri) saved = true;
  require(saved, "Native bookmark retains SMB URI");
  remote.command("Up"); settle(remote);
  bool listedShare = false;
  for (auto &entry : remote.currentScene().entries)
    if (entry.networkUri == uri && entry.directory) listedShare = true;
  if (!listedShare) {
    for (auto &entry : remote.currentScene().entries) std::cerr << "Share URI: " << entry.networkUri << '\n';
    for (int i = 0; i < remote.children(); ++i) {
      auto child = remote.child(i);
      if (child->label() && child->y() > remote.h()-50) std::cerr << "Share status: " << child->label() << '\n';
    }
  }
  require(listedShare, "Up lists the server's shares in the native scene");
  remote.load(fsv::locationArgument(uri)); settle(remote);
  // Wayland clipboard ownership needs a real seat serial, not a programmatic
  // menu callback. Inject it only into this test's private nested host display.
  if (auto display = getenv("FSVR_SMB_TEST_HOST_DISPLAY")) {
    remote.command("Select all"); remote.command("Copy");
    auto connection = XOpenDisplay(display);
    require(connection, "Private Xvfb display");
    auto xtst = dlopen("libXtst.so.6", RTLD_NOW);
    require(xtst, "Private pointer fixture");
    auto motion = reinterpret_cast<int (*)(Display *, int, int, int, unsigned long)>(dlsym(xtst, "XTestFakeMotionEvent"));
    auto button = reinterpret_cast<int (*)(Display *, unsigned int, int, unsigned long)>(dlsym(xtst, "XTestFakeButtonEvent"));
    require(motion && button, "XTest functions");
    motion(connection, -1, 400, 350, 0); button(connection, 1, True, 0); button(connection, 1, False, 0); XFlush(connection);
    for (int i = 0; i < 20; ++i) Fl::wait(.02);
    XCloseDisplay(connection); dlclose(xtst);
  }
  remote.command("Select all"); remote.command("Copy");
  if (getenv("FSVR_SMB_TEST_HOST_DISPLAY")) {
    // The application uses one Browser with tabs. Two Browser objects on one
    // Wayland connection would create competing data devices for this fixture.
    remote.load({local}); settle(remote); remote.command("Paste"); settle(remote);
    remote.load(fsv::locationArgument(uri)); settle(remote);
    destination.load({local}); settle(destination);
  } else {
    destination.load({local}); destination.show(); settle(destination);
    destination.command("Paste"); settle(destination);
  }
  if (!fs::exists(local / "český 日本語.txt")) {
    for (int i = 0; i < destination.children(); ++i) {
      auto child = destination.child(i);
      if (child->label() && child->y() > destination.h()-50) std::cerr << "Transfer status: " << child->label() << '\n';
    }
    for (auto &entry : remote.currentScene().entries) std::cerr << "Source URI: " << fsv::fileUri(entry.path) << '\n';
  }
  require(fs::exists(local / "český 日本語.txt"), "Native clipboard downloads SMB files");
  std::ofstream(local / "upload-from-ui.txt") << "native upload";
  destination.command("Refresh"); settle(destination);
  // Exercise the actual transfer engine across SMB/local filesystems, including
  // GIO's native SMB URI conversion and rename-no-overwrite fallback.
  fs::path mounted;
  for (auto &entry : remote.currentScene().entries) if (entry.name == "český 日本語.txt") mounted = entry.path.parent_path();
  require(!mounted.empty(), "Mounted directory");
  auto copied = fsv::transfer({{local / "upload-from-ui.txt"}, false}, mounted);
  require(copied.size() == 1 && fs::exists(server / "upload-from-ui.txt"), "Transfer engine uploads to SMB");
  fsv::renameFile(mounted / "upload-from-ui.txt", "renamed-from-ui.txt");
  require(fs::exists(server / "renamed-from-ui.txt"), "Native rename on SMB");
  require(fsv::localFile(fsv::fileUri(mounted / "český 日本語.txt")) == mounted / "český 日本語.txt", "Mounted file URI round trip");
  auto token = std::make_shared<std::atomic_bool>(true);
  bool cancelled = false;
  try { fsv::TransferOptions options; options.cancel = token; fsv::transfer({{local / "upload-from-ui.txt"}, false}, mounted, options); }
  catch (...) { cancelled = true; }
  require(cancelled, "SMB transfer cancellation");
  remote.command("Home"); settle(remote);
  remote.load(fsv::locationArgument(uri)); settle(remote);
  require(!remote.currentScene().entries.empty(), "Reconnect from stable URL");
  remote.command("Remove bookmark");
  remote.hide(); destination.hide();
  std::cout << "PASS native SMB scene, authentication UI, bookmark, clipboard download, upload, rename, URI round trip, cancel and reconnect\n";
} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
