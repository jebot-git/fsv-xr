// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "model.h"
namespace fsv {
struct TrashLocation {
  fs::path directory, top;
  bool home = false;
};
std::vector<TrashLocation> trashLocations();
Result scanTrash(const std::vector<TrashLocation> &, const std::shared_ptr<std::atomic_bool> &,
                 size_t limit = 8192);
void moveToTrash(const fs::path &);
void restoreTrash(const Entry &);
void eraseTrash(const Entry &);
void emptyTrash(const std::vector<TrashLocation> &, const std::shared_ptr<std::atomic_bool> &);
inline void emptyTrash(const std::shared_ptr<std::atomic_bool> &cancel) {
  emptyTrash(trashLocations(), cancel);
}
struct Place {
  std::string name;
  Location location;
  bool editable = false;
};
std::vector<Place> places();
void bookmark(const Location &, const std::string &name);
bool removeBookmark(const Location &);
std::string fileUri(const fs::path &);
fs::path localFile(const std::string &);
struct FileList {
  std::vector<fs::path> paths;
  bool cut = false;
};
std::string uriList(const FileList &);
FileList parseFiles(const std::string &bytes, const std::string &mime);
enum class Collision { Fail, Skip, KeepBoth };
struct TransferOptions {
  std::shared_ptr<std::atomic_bool> cancel;
  std::function<void(uint64_t, size_t)> progress;
  std::function<Collision(const fs::path &)> collision;
  std::function<void(const fs::path &)> completed;
};
// Completed source paths; collisions chosen as Skip are not included.
std::vector<fs::path> transfer(const FileList &, const fs::path &destination,
                               const TransferOptions & = {});
void renameFile(const fs::path &, const std::string &name);
} // namespace fsv
