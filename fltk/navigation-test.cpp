// SPDX-License-Identifier: MIT
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
static void require(bool value, const char *what) {
  if (!value)
    throw std::runtime_error(what);
}
static void wait(Browser &browser) {
  auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!browser.idle()) {
    require(std::chrono::steady_clock::now() < until, "Navigation timed out");
    Fl::wait(.005);
  }
  Fl::check();
}
int main(int argc, char **argv) try {
  char temporary[] = "/tmp/fsvr-navigation-XXXXXX";
  require(mkdtemp(temporary), "Fixture");
  fs::path root = temporary;
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  for (int i = 0; i < 20; ++i) {
    auto dir = root / ("folder-" + std::to_string(i));
    fs::create_directories(dir / "nested");
    for (int j = 0; j < 10; ++j)
      std::ofstream(dir / ("file-" + std::to_string(j) + ".txt")) << j;
  }
  dx::init();
  fl_register_images();
  Browser browser;
  browser.show();
  for (int i = 0; i < 80; ++i) {
    std::cout << "navigation " << i << std::endl;
    browser.setLayout(i % 3 == 0 ? "tree" : i % 3 == 1 ? "map" : "disc");
    browser.load({root});
    wait(browser);
    auto &scene = browser.currentScene();
    int selected = -1;
    for (size_t n = 0; n < scene.entries.size(); ++n)
      if (scene.entries[n].directory && scene.entries[n].parent == -1) {
        selected = n;
        break;
      }
    require(selected >= 0, "Folder missing");
    SpatialView *view = nullptr;
    std::function<void(Fl_Group &)> find = [&](Fl_Group &g) {
      for (int n = 0; n < g.children(); ++n) {
        if (auto v = dynamic_cast<SpatialView *>(g.child(n)))
          view = v;
        else if (auto nested = dynamic_cast<Fl_Group *>(g.child(n)))
          find(*nested);
      }
    };
    find(browser);
    require(view, "Spatial view");
    view->make_current();
    view->draw();
    view->selectionChanged(selected);
    browser.command("Open");
    wait(browser);
    browser.command("Up");
    wait(browser);
    browser.command("Back");
    wait(browser);
    browser.command("Forward");
    wait(browser);
    for (int n = 0; n < 8; ++n)
      browser.load({root / ("folder-" + std::to_string(n))});
    wait(browser);
    browser.command("New tab");
    browser.command("Close tab");
    wait(browser);
    if (i % 10 == 0)
      browser.load({root / "missing"});
    wait(browser);
  }
  if (argc > 1)
    browser.smoke(argv[1]);
  browser.hide();
  fs::remove_all(root);
  std::cout << "PASS repeated directory navigation, layouts, rapid pending "
               "scans, history and tabs\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
