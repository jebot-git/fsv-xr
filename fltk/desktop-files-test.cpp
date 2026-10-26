// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop-files.h"
#include <fstream>
#include <gio/gio.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace fsv;
void require(bool ok, const char *label) {
  if (!ok)
    throw std::runtime_error(label);
}
template <class F> void rejects(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  require(failed, "Expected rejection");
}
std::string readText(const fs::path &p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), {}};
}
int main() try {
  char temp[] = "/tmp/fsvr-desktop-XXXXXX";
  require(mkdtemp(temp), "Temporary directory");
  fs::path root = temp;
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{root};
  setenv("HOME", temp, 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  auto source = root / "source", destination = root / "destination";
  fs::create_directories(source / "folder");
  fs::create_directories(destination);
  auto special = source / "český # %\n日本語.txt";
  std::ofstream(special) << "fixture";
  FileList files{{special}, true};
  auto decoded = parseFiles("cut\n" + uriList(files), "x-special/gnome-copied-files");
  require(decoded.cut && decoded.paths == files.paths, "URI and GNOME clipboard roundtrip");
  rejects([&] { parseFiles("file://remote.invalid/a\r\n", "text/uri-list"); });
  rejects([&] { parseFiles("https://example.invalid/a\r\n", "text/uri-list"); });
  std::ofstream(source / "folder/.hidden") << "hidden";
  fs::create_directory_symlink("../folder", source / "folder/link");
  auto copied = transfer({{source / "folder", source / "folder/.hidden"}}, destination);
  require(copied.size() == 1 && readText(destination / "folder/.hidden") == "hidden" &&
              fs::is_symlink(destination / "folder/link"),
          "Recursive copy, nested selection and no symlink recursion");
  rejects([&] { transfer({{source / "folder"}}, source / "folder"); });
  rejects([&] { transfer({{source / "folder"}}, destination); });
  TransferOptions options;
  options.collision = [](auto &) { return Collision::KeepBoth; };
  transfer({{source / "folder"}}, destination, options);
  require(fs::exists(destination / "folder (copy 1)/.hidden"), "Keep both collision policy");
  options.collision = [](auto &) { return Collision::Skip; };
  require(transfer({{source / "folder"}}, destination, options).empty(), "Skip collision policy");
  transfer(files, destination);
  require(!fs::exists(special) && readText(destination / special.filename()) == "fixture",
          "Same-filesystem cut");
  fs::create_symlink("missing", source / "collision");
  std::ofstream(source / "rename") << "keep";
  rejects([&] { renameFile(source / "rename", "collision"); });
  require(readText(source / "rename") == "keep" && fs::is_symlink(source / "collision"),
          "Rename does not overwrite broken symlinks");
  rejects([&] { renameFile(source / "rename", "../bad"); });
  std::ofstream(source / "large", std::ios::binary) << std::string(4 * 1024 * 1024, 'x');
  options = {};
  options.cancel = std::make_shared<std::atomic_bool>(false);
  options.progress = [&](uint64_t bytes, size_t) {
    if (bytes)
      *options.cancel = true;
  };
  rejects([&] { transfer({{source / "large"}}, destination, options); });
  require(fs::exists(source / "large") && !fs::exists(destination / "large"),
          "Cancelled copy removes partial target and retains source");
  std::ofstream(source / "first") << "moved";
  std::ofstream(source / "second") << "retained";
  std::ofstream(destination / "second") << "collision";
  options = {};
  std::vector<fs::path> finishedSources;
  options.completed = [&](const fs::path &p) { finishedSources.push_back(p); };
  rejects([&] { transfer({{source / "first", source / "second"}, true}, destination, options); });
  require(finishedSources == std::vector<fs::path>{source / "first"} &&
              !fs::exists(source / "first") && fs::exists(source / "second"),
          "Partial moves report completed sources before a later failure");
  fs::create_directories(source / "readonly/nested");
  std::ofstream(source / "readonly/nested/file") << "readonly";
  fs::permissions(source / "readonly/nested", fs::perms::owner_read | fs::perms::owner_exec);
  options = {};
  options.cancel = std::make_shared<std::atomic_bool>(false);
  options.progress = [&](uint64_t, size_t count) {
    if (count == 2)
      *options.cancel = true;
  };
  rejects([&] { transfer({{source / "readonly"}}, destination, options); });
  require(!fs::exists(destination / "readonly") && fs::exists(source / "readonly/nested/file"),
          "Cancelled copy rolls back completed read-only subdirectories");
  fs::permissions(source / "readonly/nested", fs::perms::owner_all);
  char shared[] = "/dev/shm/fsvr-cross-move-XXXXXX";
  if (mkdtemp(shared)) {
    Cleanup sharedCleanup{shared};
    struct stat a{}, b{};
    stat(source.c_str(), &a);
    stat(shared, &b);
    if (a.st_dev != b.st_dev) {
      std::ofstream(source / "cross") << "cross filesystem";
      transfer({{source / "cross"}, true}, shared);
      require(!fs::exists(source / "cross") &&
                  readText(fs::path(shared) / "cross") == "cross filesystem",
              "Cross-filesystem cut");
      std::ofstream(source / "changing") << "old";
      options = {};
      options.progress = [&](uint64_t bytes, size_t) {
        if (bytes)
          std::ofstream(source / "changing") << "changed while copying";
      };
      rejects([&] { transfer({{source / "changing"}, true}, shared, options); });
      require(fs::exists(source / "changing") && !fs::exists(fs::path(shared) / "changing"),
              "Changed cross-filesystem source is retained");
    }
  }
  bookmark({source}, "Bookmark 日本語");
  require(removeBookmark({source}), "Remove own bookmark");
  require(!removeBookmark({source}), "Removing absent bookmark");
  bookmark({source}, "Bookmark 日本語");
  bool found = false;
  for (auto &p : places())
    if (p.location.path == source && p.editable && p.name == "Bookmark 日本語")
      found = true;
  require(found, "Own places preserve Unicode");
  fs::create_directories(root / "config/gtk-3.0");
  std::ofstream(root / "config/gtk-3.0/bookmarks") << fileUri(destination) << " GTK fixture\n";
  found = false;
  for (auto &p : places())
    if (p.location.path == destination && p.name == "GTK fixture")
      found = true;
  require(found, "GTK bookmarks imported");
  auto original = source / "trash # 日本語.txt";
  std::ofstream(original) << "trashed";
  moveToTrash(original);
  TrashLocation store{root / "data/Trash", root / "data", true};
  auto token = std::make_shared<std::atomic_bool>(false);
  auto trash = scanTrash({store}, token);
  require(trash.entries.size() == 1 && trash.entries[0].trash->original == original,
          "Actual GIO Trash preserves original location");
  auto record = trash.entries[0];
  std::ofstream(original) << "new";
  rejects([&] { restoreTrash(record); });
  require(readText(original) == "new", "Restore refuses a collision");
  fs::remove(original);
  restoreTrash(record);
  require(readText(original) == "trashed", "Restore returns the original file");
  moveToTrash(original);
  trash = scanTrash({store}, token);
  record = trash.entries[0];
  std::ofstream(record.path) << "tampered and longer";
  rejects([&] { restoreTrash(record); });
  auto outside = root / "outside";
  std::ofstream(outside) << "must survive";
  fs::create_symlink(outside, store.directory / "files/broken-info");
  fs::create_symlink(outside, store.directory / "info/broken-info.trashinfo");
  trash = scanTrash({store}, token, 1);
  require(trash.entries.size() == 1, "Trash view limit");
  emptyTrash({store}, token);
  require(scanTrash({store}, token).entries.empty() && readText(outside) == "must survive",
          "Empty Trash covers hidden/damaged entries without following symlinks");
  fs::path volume = root / "mounted";
  TrashLocation external{volume / ".Trash-test", volume, false};
  fs::create_directories(external.directory / "files");
  fs::create_directories(external.directory / "info");
  chmod(external.directory.c_str(), 0700);
  chmod((external.directory / "files").c_str(), 0700);
  chmod((external.directory / "info").c_str(), 0700);
  std::ofstream(external.directory / "files/a") << "external";
  std::ofstream(external.directory / "info/a.trashinfo")
      << "[Trash Info]\nPath=../escape\nDeletionDate=2026-10-26T12:00:00\n";
  trash = scanTrash({external}, token);
  require(trash.entries.size() == 1 && trash.entries[0].trash->original.empty(),
          "Traversal metadata cannot restore");
  std::ofstream(external.directory / "info/a.trashinfo")
      << "[Trash Info]\nPath=restored\nDeletionDate=2026-10-26T12:00:00\n";
  trash = scanTrash({external}, token);
  restoreTrash(trash.entries[0]);
  require(readText(volume / "restored") == "external", "Volume-relative Trash paths restore");
  require(locationArgument("trash:///").trash, "Trash URL is a virtual location");
  std::cout << "PASS local clipboard formats, recursive/cross-filesystem transfers, conflicts, "
               "cancellation, source changes, bookmarks, actual GIO Trash, restore collisions, "
               "stale records, damaged metadata and volume-relative paths\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
