// SPDX-License-Identifier: LGPL-2.1-or-later
#include "model.h"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace fsv;
static void require(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
static void malicious(const fs::path &p, const char *name) {
  auto a = archive_write_new();
  archive_write_set_format_pax_restricted(a);
  archive_write_open_filename(a, p.c_str());
  auto e = archive_entry_new();
  archive_entry_set_pathname(e, name);
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  archive_entry_set_size(e, 1);
  archive_write_header(a, e);
  archive_write_data(a, "x", 1);
  archive_entry_free(e);
  archive_write_close(a);
  archive_write_free(a);
}
static void sortingTests() {
  auto node = [](const char *name, int parent, bool directory, uint64_t size,
                 const char *type, int64_t date) {
    Entry e;
    e.name = name;
    e.path = fs::path("/fixture") / name;
    e.parent = parent;
    e.directory = directory;
    e.bytes = size;
    e.type = type;
    e.modified = date;
    e.accessed = date + 10;
    e.created = date + 20;
    return e;
  };
  std::vector<Entry> input = {node("Folder10", -1, true, 30, "Folder", 10),
                              node("folder2", -1, true, 20, "Folder", 20),
                              node("File10.txt", -1, false, 10, "Text", 30),
                              node("file2.png", -1, false, 20, "Image", 40),
                              node("z.txt", 0, false, 30, "Text", 50),
                              node("a.txt", 1, false, 20, "Text", 60)};
  SortOptions options;
  auto sorted = sortEntries(input, options);
  require(sorted[0].name == "folder2" && sorted[1].name == "Folder10" &&
              sorted[2].name == "file2.png" && sorted[3].name == "File10.txt",
          "Natural case-insensitive names and folders first");
  for (auto &e : sorted)
    if (e.parent >= 0)
      require(sorted[e.parent].directory &&
                  ((e.name == "a.txt" && sorted[e.parent].name == "folder2") ||
                   (e.name == "z.txt" && sorted[e.parent].name == "Folder10")),
              "Reordering remaps child identities correctly");
  options.descending = true;
  sorted = sortEntries(input, options);
  require(sorted[0].directory && sorted[1].directory &&
              sorted[0].name == "Folder10" && sorted[2].name == "File10.txt",
          "Descending keeps folder grouping independent of direction");
  options.key = SortKey::Size;
  sorted = sortEntries(input, options);
  require(sorted[0].name == "Folder10" && sorted[2].name == "file2.png",
          "Largest-first sizes");
  options.descending = false;
  sorted = sortEntries(input, options);
  require(sorted[0].name == "folder2" && sorted[2].name == "File10.txt",
          "Smallest-first sizes");
  options.foldersFirst = false;
  options.descending = true;
  options.key = SortKey::Modified;
  sorted = sortEntries(input, options);
  require(sorted[0].name == "file2.png", "Newest-first timestamps");
  input[2].modified = {};
  sorted = sortEntries(input, options);
  require(sorted[3].name == "File10.txt", "Unknown dates last descending");
  options.descending = false;
  sorted = sortEntries(input, options);
  require(sorted[3].name == "File10.txt", "Unknown dates last ascending");
  options.key = SortKey::Type;
  sorted = sortEntries(input, options);
  require(sorted[2].name == "file2.png" && sorted[3].name == "File10.txt",
          "Types use human-readable file type names");
  options.key = SortKey::Extension;
  sorted = sortEntries(input, options);
  require(sorted[2].name == "file2.png", "Extensions sort independently");
  for (auto key : {SortKey::Created, SortKey::Accessed}) {
    options.key = key;
    options.descending = true;
    sorted = sortEntries(input, options);
    require(sorted[0].name == "file2.png", "Created/accessed timestamps sort");
  }
  auto cases = std::vector<Entry>{node("apple", -1, false, 0, "File", 0),
                                  node("Banana", -1, false, 0, "File", 0),
                                  node("Ä2", -1, false, 0, "File", 0),
                                  node("ä10", -1, false, 0, "File", 0)};
  options = {};
  options.foldersFirst = false;
  auto insensitive = sortEntries(cases, options);
  require(insensitive[0].name == "apple", "Ignore-case ordering");
  auto pos = [&](const char *name) {
    return std::find_if(insensitive.begin(), insensitive.end(),
                        [&](auto &e) { return e.name == name; }) -
           insensitive.begin();
  };
  require(pos("Ä2") < pos("ä10"),
          "UTF-8 case folding with natural digit groups");
  options.caseSensitive = true;
  auto sensitive = sortEntries(cases, options);
  require(sensitive[0].name == "Banana", "Case-sensitive ordering");
  for (auto layout : {"map", "tree", "disc"}) {
    auto scene = arrange(sortEntries(input, options), layout);
    for (int i = 0; i < int(scene.entries.size()); ++i)
      require(scene.entries[i].parent < i,
              "Sorted layouts retain valid parents");
  }
}
int main() try {
  sortingTests();
  char pattern[] = "/tmp/fsv-fltk-core-XXXXXX";
  auto p = mkdtemp(pattern);
  require(p, "Temporary directory");
  fs::path root = p;
  struct Cleanup {
    fs::path p;
    ~Cleanup() { fs::remove_all(p); }
  } cleanup{root};
  fs::create_directories(root / "source/a/b");
  std::ofstream(root / "source/one.txt") << "12345";
  std::ofstream(root / "source/a/two.txt") << "1234567";
  fs::create_directory_symlink(root / "source", root / "source/loop");
  auto token = std::make_shared<std::atomic_bool>(false);
  auto result = scan({root / "source"}, token);
  require(result.entries.size() == 5,
          "Nested scan does not follow directory symlinks");
  for (auto &e : result.entries) {
    require(e.modified.has_value() && e.accessed.has_value() && !e.type.empty(),
            "Filesystem metadata available for sorting");
    if (e.name == "a")
      require(e.bytes == 7, "Directory sizes include descendants");
  }
  for (auto layout : {"map", "tree", "disc"}) {
    auto scene = arrange(result.entries, layout);
    for (auto &e : scene.entries)
      require(std::isfinite(e.center.x) && std::isfinite(e.center.z) &&
                  e.extent.x > 0 && e.extent.y > 0 && e.extent.z > 0,
              "Every layout has finite positive geometry");
    if (std::string(layout) == "tree")
      require(!scene.branches.empty() && scene.root.extent.x > 0,
              "TreeV generates root platforms and branches");
    auto e = scene.entries.back();
    require(pick(scene, {e.center.x, e.center.y + e.extent.y + 1, e.center.z},
                 {0, -1, 0}) >= 0,
            "Ray picker hits rendered geometry");
  }
  auto bounded = scan({root / "source"}, token, 0, 2);
  require(bounded.entries.size() <= 2 && !bounded.warning.empty(),
          "Scans enforce entry/depth limits");
  *token = true;
  require(scan({root / "source"}, token).entries.empty(),
          "Cancelled scan returns promptly");
  *token = false;
  compress(root / "source", root / "safe.zip");
  auto archive = scan({root / "safe.zip", "", true}, token);
  require(!archive.entries.empty(),
          "Compressed files can be browsed without extraction");
  for (auto &e : archive.entries)
    require(!e.type.empty(), "Archive members have file types");
  auto sortedArchive = sortEntries(
      archive.entries, SortOptions{SortKey::Modified, true, true, false});
  require(sortedArchive.size() == archive.entries.size(),
          "Archive metadata sorting retains all members");
  auto material = materialize({root / "safe.zip", "source/a/two.txt", true},
                              root / "preview");
  require(fs::file_size(material) == 7,
          "A single archive member materializes accurately");
  auto extracted = extract(root / "safe.zip");
  require(fs::file_size(extracted / "source/one.txt") == 5,
          "Safe archive extraction round-trips files");
  require(!fs::exists(extracted / "source/loop"),
          "Compression does not follow symlink loops");
  bool refused = false;
  try {
    extract(root / "safe.zip");
  } catch (...) {
    refused = true;
  }
  require(refused, "Extraction refuses existing destination");
  for (auto name : {"../../outside.txt", "/tmp/outside.txt"}) {
    malicious(root / "bad.tar", name);
    refused = false;
    try {
      extract(root / "bad.tar");
    } catch (...) {
      refused = true;
    }
    require(refused && !fs::exists(root / "bad.extracted"),
            "Archive traversal is rejected and its partial extraction removed");
  }
  std::cout << "PASS: bounded/cancellable filesystem scan, MapV/TreeV/DiscV "
               "geometry, ray picking, archive browse/materialize/round-trip "
               "and traversal rejection\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
