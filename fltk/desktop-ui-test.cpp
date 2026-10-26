// SPDX-License-Identifier: LGPL-2.1-or-later
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <iostream>
void require(bool okay, const char *s) {
  if (!okay)
    throw std::runtime_error(s);
}
template <class T> T *findWidget(Fl_Group &g) {
  for (int i = 0; i < g.children(); ++i) {
    if (auto w = dynamic_cast<T *>(g.child(i)))
      return w;
    if (auto child = dynamic_cast<Fl_Group *>(g.child(i)))
      if (auto w = findWidget<T>(*child))
        return w;
  }
  return nullptr;
}
void settle() {
  for (int i = 0; i < 20; ++i)
    Fl::wait(.02);
}
std::string readText(const fs::path &p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), {}};
}
void acceptDelete(void *) {
  if (auto modal = Fl::modal()) {
    for (int i = 0; i < modal->children(); ++i)
      if (auto b = dynamic_cast<Fl_Button *>(modal->child(i)))
        if (b->label() && std::string(b->label()) == "Delete") {
          b->do_callback();
          return;
        }
  }
  Fl::repeat_timeout(.05, acceptDelete);
}
int main(int argc, char **argv) try {
  char temporary[] = "/tmp/fsvr-desktop-ui-XXXXXX";
  require(mkdtemp(temporary), "Temporary fixture");
  fs::path root = temporary;
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{root};
  setenv("HOME", temporary, 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  auto source = root / "source", destination = root / "destination", moved = root / "moved";
  fs::create_directories(source / "folder");
  fs::create_directories(destination);
  fs::create_directories(moved);
  std::ofstream(source / "český # 日本語.txt") << "Unicode transfer";
  std::ofstream(source / ".hidden") << "hidden";
  std::ofstream(source / "folder/nested") << "nested";
  dx::init();
  fl_register_images();
  Browser a, b;
  a.load({source});
  b.load({destination});
  a.show();
  b.show();
  a.smoke(nullptr);
  b.smoke(nullptr);
  settle();
  a.command("Select all");
  a.command("Copy");
  b.command("Paste");
  b.smoke(nullptr);
  settle();
  require(readText(destination / "český # 日本語.txt") == "Unicode transfer" &&
              fs::exists(destination / ".hidden") && fs::exists(destination / "folder/nested"),
          "Actual native clipboard copies a multiselection between windows");
  b.command("Select all");
  b.command("Cut");
  a.load({moved});
  a.smoke(nullptr);
  a.command("Paste");
  a.smoke(nullptr);
  settle();
  require(fs::is_empty(destination) && fs::exists(moved / "folder/nested"),
          "Cut data moves files only after successful paste");
  a.command("Add bookmark");
  settle();
  auto sidebar = findWidget<Sidebar>(a);
  require(sidebar, "Sidebar");
  bool found = false;
  for (int i = 1; i <= sidebar->places.size(); ++i)
    if (std::string(sidebar->places.text(i)) == "moved")
      found = true;
  require(found, "Places shows new bookmark");
  if (argc > 1)
    a.smoke((std::string(argv[1]) + ".places.ppm").c_str());
  a.command("Remove bookmark");
  auto original = source / "trash restore #.txt";
  std::ofstream(original) << "Restore fixture";
  fsv::moveToTrash(original);
  a.command("Trash");
  a.smoke(nullptr);
  settle();
  require(a.currentScene().entries.size() == 1, "Trash URL loads native virtual scene");
  a.command("Select all");
  if (argc > 1)
    a.smoke((std::string(argv[1]) + ".trash.ppm").c_str());
  a.command("Open in new tab");
  require(a.currentScene().entries[0].trash.has_value(),
          "Trash cannot be opened as a writable physical directory");
  a.command("Restore from Trash");
  a.smoke(nullptr);
  settle();
  require(readText(original) == "Restore fixture" && a.currentScene().entries.empty(),
          "Native Restore action returns the file");
  fsv::moveToTrash(original);
  a.command("Refresh");
  a.smoke(nullptr);
  settle();
  require(a.currentScene().entries.size() == 1, "Refreshed Trash");
  // Empty only the isolated home store: no physical mounted-volume stores in this test.
  fsv::emptyTrash({{root / "data/Trash", root / "data", true}},
                  std::make_shared<std::atomic_bool>(false));
  a.command("Refresh");
  a.smoke(nullptr);
  require(a.currentScene().entries.empty(), "Empty virtual view after deletion");
  if (argc > 1)
    a.smoke(argv[1]);
  a.hide();
  b.hide();
  std::cout << "PASS native Places, multiselection, real clipboard copy/cut between windows, "
               "virtual Trash navigation and restore\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
