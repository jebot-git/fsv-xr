// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "archive-ops.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace fsv {
namespace fs = std::filesystem;
struct Vec {
  double x = 0, y = 0, z = 0;
  Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec operator*(double n) const { return {x * n, y * n, z * n}; }
};
struct Entry {
  fs::path path;
  std::string name, member;
  uint64_t bytes = 0;
  std::optional<int64_t> modified, accessed, created;
  std::string type;
  int parent = -1, depth = 0;
  bool directory = false, symlink = false;
  Vec center, extent;
};
enum class SortKey { Name, Size, Modified, Type, Created, Accessed, Extension };
struct SortOptions {
  SortKey key = SortKey::Name;
  bool descending = false, foldersFirst = true, caseSensitive = false;
};
const char *sortKeyName(SortKey);
std::optional<SortKey> sortKey(const std::string &);
// Sort each sibling group while retaining parent-before-child order.
std::vector<Entry> sortEntries(std::vector<Entry>, const SortOptions &);
struct Result {
  std::vector<Entry> entries;
  std::string warning;
};
struct Location {
  fs::path path;
  std::string member = "";
  bool archive = false;
  std::string password = "";
  std::string display() const {
    return path.string() + (archive ? "!/" + member : "");
  }
  bool operator==(const Location &b) const {
    return path == b.path && member == b.member && archive == b.archive;
  }
};
struct Scene {
  std::vector<Entry> entries;
  Entry root;
  std::vector<std::pair<Vec, Vec>> branches;
};
Result scan(const Location &, const std::shared_ptr<std::atomic_bool> &cancel,
            int depth = 3, size_t limit = 8192);
Scene arrange(std::vector<Entry>, const std::string &layout);
bool archiveSupported(const fs::path &);
std::string safeMember(const std::string &);
Location locationArgument(const std::string &);
fs::path materialize(const Location &, const fs::path &temporary, const ArchiveOptions & = {});
fs::path extract(const fs::path &archive);
void compress(const fs::path &source, const fs::path &destination);
int pick(const Scene &, Vec origin, Vec direction);
} // namespace fsv
