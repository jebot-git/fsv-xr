// SPDX-License-Identifier: LGPL-2.1-or-later
// Toolkit-neutral port of the original fsvr scan and TreeV/MapV/DiscV layouts.
#include "model.h"
#include "desktop-files.h"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <gio/gio.h>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace fsv {
namespace {
bool stop(const std::shared_ptr<std::atomic_bool> &c) { return c && c->load(); }
std::string contentType(const Entry &e) {
  if (e.directory)
    return "Folder";
  if (e.symlink)
    return "Symbolic link";
  gboolean unsure = false;
  char *mime = g_content_type_guess(e.name.c_str(), nullptr, 0, &unsure);
  char *description = mime ? g_content_type_get_description(mime) : nullptr;
  std::string result = description ? description : "File";
  g_free(description);
  g_free(mime);
  return result;
}
std::string collation(const std::string &value, bool sensitive) {
  if (!g_utf8_validate(value.c_str(), value.size(), nullptr))
    return value;
  char *folded = sensitive ? g_strdup(value.c_str())
                           : g_utf8_casefold(value.c_str(), value.size());
  char *key = g_utf8_collate_key_for_filename(folded, -1);
  std::string result(key);
  g_free(key);
  g_free(folded);
  return result;
}
std::optional<int64_t> timestamp(int64_t seconds, int64_t nanos) {
  constexpr int64_t scale = 1000000000;
  if (seconds > std::numeric_limits<int64_t>::max() / scale - 1 ||
      seconds < std::numeric_limits<int64_t>::min() / scale + 1 || nanos < 0 ||
      nanos >= scale)
    return {};
  return seconds * scale + nanos;
}
int compareTime(const std::optional<int64_t> &a,
                const std::optional<int64_t> &b) {
  return a && b ? (*a > *b) - (*a < *b) : 0;
}
void sum(Result &r) {
  for (int i = int(r.entries.size()) - 1; i >= 0; --i)
    if (r.entries[i].parent >= 0) {
      auto &total = r.entries[r.entries[i].parent].bytes;
      total = std::min(std::numeric_limits<uint64_t>::max() - total,
                       r.entries[i].bytes) +
              total;
    }
}
} // namespace
const char *sortKeyName(SortKey key) {
  switch (key) {
  case SortKey::Name:
    return "name";
  case SortKey::Size:
    return "size";
  case SortKey::Modified:
    return "modified";
  case SortKey::Type:
    return "type";
  case SortKey::Created:
    return "created";
  case SortKey::Accessed:
    return "accessed";
  case SortKey::Extension:
    return "extension";
  }
  return "name";
}
std::optional<SortKey> sortKey(const std::string &name) {
  for (auto key :
       {SortKey::Name, SortKey::Size, SortKey::Modified, SortKey::Type,
        SortKey::Created, SortKey::Accessed, SortKey::Extension})
    if (name == sortKeyName(key))
      return key;
  return {};
}
std::vector<Entry> sortEntries(std::vector<Entry> entries,
                               const SortOptions &options) {
  std::vector<std::vector<int>> children(entries.size() + 1);
  struct Keys {
    std::string name, type, extension;
  };
  std::vector<Keys> keys;
  for (int i = 0; i < int(entries.size()); ++i) {
    auto &e = entries[i];
    if (e.parent < -1 || e.parent >= i)
      throw std::runtime_error("Invalid filesystem parent");
    children[e.parent + 1].push_back(i);
    keys.push_back({collation(e.name, options.caseSensitive),
                    collation(e.type, false),
                    collation(fs::path(e.name).extension().string(),
                              options.caseSensitive)});
  }
  auto time = [&](const Entry &e) {
    return options.key == SortKey::Modified  ? e.modified
           : options.key == SortKey::Created ? e.created
                                             : e.accessed;
  };
  for (auto &siblings : children)
    std::stable_sort(siblings.begin(), siblings.end(), [&](int ai, int bi) {
      const auto &a = entries[ai], &b = entries[bi];
      const auto &ak = keys[ai], &bk = keys[bi];
      if (options.foldersFirst && a.directory != b.directory)
        return a.directory;
      int order = 0;
      switch (options.key) {
      case SortKey::Name:
        order = ak.name.compare(bk.name);
        break;
      case SortKey::Size:
        order = (a.bytes > b.bytes) - (a.bytes < b.bytes);
        break;
      case SortKey::Type:
        order = ak.type.compare(bk.type);
        break;
      case SortKey::Extension:
        order = ak.extension.compare(bk.extension);
        break;
      default:
        // Unavailable dates always sort last, independently of direction.
        if (bool(time(a)) != bool(time(b)))
          return bool(time(a));
        order = compareTime(time(a), time(b));
        break;
      }
      if (!order)
        order = ak.name.compare(bk.name);
      if (!order)
        order = a.name.compare(b.name);
      if (!order)
        order = a.member.compare(b.member);
      if (!order)
        order = a.path.compare(b.path);
      return options.descending ? order > 0 : order < 0;
    });
  // Breadth-first output is bounded and does not recurse on deep input trees.
  std::vector<int> queue = children[0], remap(entries.size(), -1);
  std::vector<Entry> result;
  result.reserve(entries.size());
  for (size_t i = 0; i < queue.size(); ++i) {
    int old = queue[i];
    auto e = std::move(entries[old]);
    e.parent = e.parent < 0 ? -1 : remap[e.parent];
    remap[old] = result.size();
    result.push_back(std::move(e));
    queue.insert(queue.end(), children[old + 1].begin(),
                 children[old + 1].end());
  }
  return result;
}
std::string safeMember(const std::string &raw) {
  if (raw.empty() || raw.size() > 4096 || raw.find('\0') != std::string::npos ||
      raw.find('\\') != std::string::npos)
    throw std::runtime_error("Invalid archive member");
  fs::path p(raw);
  if (p.is_absolute() || p.has_root_name() || (raw.size() > 1 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':'))
    throw std::runtime_error("Absolute archive member blocked");
  for (auto &part : p)
    if (part == "..")
      throw std::runtime_error("Archive traversal blocked");
  auto result = p.lexically_normal().generic_string();
  while (!result.empty() && result.back() == '/')
    result.pop_back();
  if (result == "." || result.empty())
    return {};
  return result;
}
Result scan(const Location &location,
            const std::shared_ptr<std::atomic_bool> &cancel, int maxDepth,
            size_t limit) {
  Result result;
  if (location.trash)
    return scanTrash(trashLocations(), cancel, limit);
  bool truncated = false, unreadable = false;
  if (!location.archive) {
    std::function<void(fs::path, int, int)> visit = [&](fs::path path,
                                                        int parent, int depth) {
      if (stop(cancel))
        return;
      std::error_code ec;
      fs::directory_iterator it(
          path, fs::directory_options::skip_permission_denied, ec),
          end;
      if (ec) {
        unreadable = true;
        return;
      }
      // Files and symlink targets can change while this directory is scanned.
      // Never query live metadata in a sort comparator: changing answers break
      // strict weak ordering and can make std::sort walk outside its range.
      struct Candidate {
        fs::directory_entry file;
        bool directory = false, symlink = false;
      };
      std::vector<Candidate> sorted;
      while (it != end && !stop(cancel)) {
        if (sorted.size() + result.entries.size() >= limit) {
          truncated = true;
          break;
        }
        Candidate candidate{*it};
        candidate.directory = candidate.file.is_directory(ec);
        candidate.symlink = candidate.file.is_symlink(ec);
        sorted.push_back(std::move(candidate));
        it.increment(ec);
        if (ec) {
          unreadable = true;
          break;
        }
      }
      std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) {
        return a.directory != b.directory
                   ? a.directory > b.directory
                   : a.file.path().filename() < b.file.path().filename();
      });
      std::vector<int> dirs;
      for (auto &candidate : sorted) {
        auto &file = candidate.file;
        if (result.entries.size() >= limit) {
          truncated = true;
          break;
        }
        Entry e;
        e.path = file.path();
        e.name = file.path().filename().string();
        e.parent = parent;
        e.depth = depth;
        e.directory = candidate.directory;
        e.symlink = candidate.symlink;
        if (!e.directory) {
          auto n = file.file_size(ec);
          e.bytes = ec ? 0 : n;
        }
        struct stat st{};
        if (!lstat(e.path.c_str(), &st)) {
          e.modified = timestamp(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
          e.accessed = timestamp(st.st_atim.tv_sec, st.st_atim.tv_nsec);
        }
#ifdef __linux__
        struct statx sx{};
        if (!statx(AT_FDCWD, e.path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME,
                   &sx) &&
            (sx.stx_mask & STATX_BTIME))
          e.created = timestamp(sx.stx_btime.tv_sec, sx.stx_btime.tv_nsec);
#endif
        e.type = contentType(e);
        int index = result.entries.size();
        result.entries.push_back(e);
        if (e.directory && !e.symlink)
          dirs.push_back(index);
      }
      for (int i : dirs)
        if (depth < maxDepth)
          visit(result.entries[i].path, i, depth + 1);
        else
          truncated = true;
    };
    visit(location.path, -1, 0);
  } else {
    ArchiveReader reader(location.path, location.password);
    std::map<std::string, Entry> index;
    archive_entry *item = nullptr;
    size_t bytes = 0;
    while (!stop(cancel) &&
           reader.next(item)) {
      std::string member;
      try { member = reader.member(item); }
      catch (const std::exception &) {
        unreadable = true;
        reader.skip();
        continue;
      }
      if (member.empty())
        continue;
      bytes += member.size();
      if (index.size() >= 32768 || bytes > 16 * 1024 * 1024) {
        truncated = true;
        break;
      }
      auto type = archive_entry_filetype(item);
      if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK) {
        unreadable = true;
        reader.skip();
        continue;
      }
      Entry e;
      e.path = location.path;
      e.member = member;
      e.name = fs::path(member).filename().string();
      e.directory = type == AE_IFDIR;
      e.symlink = type == AE_IFLNK;
      e.bytes =
          e.directory ? 0 : std::max<int64_t>(0, archive_entry_size(item));
      if (archive_entry_mtime_is_set(item))
        e.modified = timestamp(archive_entry_mtime(item),
                               archive_entry_mtime_nsec(item));
      if (archive_entry_atime_is_set(item))
        e.accessed = timestamp(archive_entry_atime(item),
                               archive_entry_atime_nsec(item));
      if (archive_entry_birthtime_is_set(item))
        e.created = timestamp(archive_entry_birthtime(item),
                              archive_entry_birthtime_nsec(item));
      e.type = contentType(e);
      index[member] = e;
      auto parent = fs::path(member).parent_path();
      while (!parent.empty()) {
        auto key = parent.generic_string();
        if (!index.count(key)) {
          Entry d;
          d.path = location.path;
          d.member = key;
          d.name = parent.filename().string();
          d.directory = true;
          d.type = "Folder";
          index[key] = d;
        }
        parent = parent.parent_path();
      }
      reader.skip();
    }
    std::string prefix =
        location.member.empty() ? "" : safeMember(location.member) + "/";
    std::map<std::string, int> ids;
    std::vector<Entry> pending;
    for (auto &kv : index) {
      auto &e = kv.second;
      if (e.member.rfind(prefix, 0) != 0 || e.member == location.member)
        continue;
      auto rel = e.member.substr(prefix.size());
      int depth = std::count(rel.begin(), rel.end(), '/');
      if (depth > maxDepth) {
        truncated = true;
        continue;
      }
      e.depth = depth;
      pending.push_back(e);
    }
    std::stable_sort(pending.begin(), pending.end(), [](auto &a, auto &b) {
      return a.depth != b.depth ? a.depth < b.depth : a.member < b.member;
    });
    for (auto &e : pending) {
      if (result.entries.size() >= limit) {
        truncated = true;
        break;
      }
      auto parent = fs::path(e.member).parent_path().generic_string();
      e.parent = parent == location.member || parent.empty()
                     ? -1
                     : (ids.count(parent) ? ids[parent] : -1);
      ids[e.member] = result.entries.size();
      result.entries.push_back(e);
    }
  }
  sum(result);
  if (truncated)
    result.warning = "View bounded to " + std::to_string(limit) +
                     " entries / " + std::to_string(maxDepth + 1) +
                     " levels; open a directory to explore further. Sizes "
                     "cover scanned entries.";
  if (unreadable)
    result.warning += " Some unreadable or unsafe entries were skipped.";
  return result;
}
Scene arrange(std::vector<Entry> entries, const std::string &layout) {
  Scene scene;
  scene.entries = std::move(entries);
  auto &e = scene.entries;
  std::vector<std::vector<int>> children(e.size() + 1), dirs(e.size() + 1),
      leaves(e.size() + 1);
  for (int i = 0; i < int(e.size()); ++i) {
    if (e[i].parent >= i || e[i].parent < -1)
      throw std::runtime_error("Invalid filesystem parent");
    children[e[i].parent + 1].push_back(i);
    (e[i].directory && !e[i].symlink ? dirs : leaves)[e[i].parent + 1]
        .push_back(i);
  }
  scene.root.directory = true;
  if (layout == "tree") {
    std::vector<int> platforms{-1}, columns(e.size() + 1, 1);
    int levels = 1;
    for (int i = 0; i < int(e.size()); ++i)
      if (e[i].directory && !e[i].symlink) {
        platforms.push_back(i);
        levels = std::max(levels, e[i].depth + 2);
      }
    std::vector<Vec> sizes(e.size() + 1);
    std::vector<double> widths(e.size() + 1), depths(levels), levelZ(levels);
    auto level = [&](int parent) {
      return parent < 0 ? 0 : e[parent].depth + 1;
    };
    for (int parent : platforms) {
      int n = leaves[parent + 1].size();
      columns[parent + 1] = std::max(1, int(std::ceil(std::sqrt(double(n)))));
      int rows = (n + columns[parent + 1] - 1) / columns[parent + 1];
      sizes[parent + 1] = {double(std::max(24, columns[parent + 1] * 16 + 8)),
                           1.8, std::max(32., (rows * 16 + 8) / .68)};
      depths[level(parent)] =
          std::max(depths[level(parent)], sizes[parent + 1].z);
    }
    for (auto it = platforms.rbegin(); it != platforms.rend(); ++it) {
      int parent = *it;
      double width = 0;
      for (int child : dirs[parent + 1])
        width += widths[child + 1] + 20;
      if (width > 0)
        width -= 20;
      widths[parent + 1] = std::max(sizes[parent + 1].x, width);
    }
    for (int n = 1; n < levels; ++n)
      levelZ[n] = levelZ[n - 1] - depths[n - 1] / 2 - 24 - depths[n] / 2;
    double minx = 1e30, minz = 1e30, maxx = -1e30, maxz = -1e30;
    std::function<void(int, double)> place = [&](int parent, double left) {
      auto size = sizes[parent + 1];
      double x = left + widths[parent + 1] / 2 - widths[0] / 2,
             z = levelZ[level(parent)];
      auto &platform = parent < 0 ? scene.root : e[parent];
      platform.center = {x, .9, z};
      platform.extent = size;
      minx = std::min(minx, x - size.x / 2);
      maxx = std::max(maxx, x + size.x / 2);
      minz = std::min(minz, z - size.z / 2);
      maxz = std::max(maxz, z + size.z / 2);
      int j = 0;
      for (int child : leaves[parent + 1]) {
        double height = 2 + std::min(15., std::log2(1. + e[child].bytes) * .55);
        e[child].extent = {12, height, 12};
        e[child].center = {x - size.x / 2 + 12 + (j % columns[parent + 1]) * 16,
                           1.8 + height / 2,
                           z - size.z / 2 + 12 +
                               (j / columns[parent + 1]) * 16};
        ++j;
      }
      double width = 0;
      for (int child : dirs[parent + 1])
        width += widths[child + 1] + 20;
      if (width > 0)
        width -= 20;
      double cursor = left + (widths[parent + 1] - width) / 2;
      for (int child : dirs[parent + 1]) {
        place(child, cursor);
        cursor += widths[child + 1] + 20;
      }
    };
    place(-1, 0);
    for (int parent : platforms) {
      auto &kids = dirs[parent + 1];
      if (kids.empty())
        continue;
      auto &node = parent < 0 ? scene.root : e[parent];
      double back = node.center.z - node.extent.z / 2, front = -1e30,
             minX = node.center.x, maxX = minX;
      for (int child : kids) {
        front = std::max(front, e[child].center.z + e[child].extent.z / 2);
        minX = std::min(minX, e[child].center.x);
        maxX = std::max(maxX, e[child].center.x);
      }
      double junction = (back + front) / 2;
      scene.branches.push_back(
          {{node.center.x, .9, back}, {node.center.x, .9, junction}});
      scene.branches.push_back({{minX, .9, junction}, {maxX, .9, junction}});
      for (int child : kids)
        scene.branches.push_back({{e[child].center.x, .9, junction},
                                  {e[child].center.x, .9,
                                   e[child].center.z + e[child].extent.z / 2}});
    }
    double factor = std::min(2.5, std::min(160. / std::max(.01, maxx - minx),
                                           200. / std::max(.01, maxz - minz)));
    auto transform = [&](Vec p) {
      return Vec{p.x - (minx + maxx) / 2, p.y, p.z - (minz + maxz) / 2} *
             factor;
    };
    for (auto &n : e) {
      n.center = transform(n.center);
      n.extent = n.extent * factor;
    }
    scene.root.center = transform(scene.root.center);
    scene.root.extent = scene.root.extent * factor;
    for (auto &line : scene.branches) {
      line.first = transform(line.first);
      line.second = transform(line.second);
    }
    return scene;
  }
  struct Rect {
    double x, z, w, d;
  };
  std::function<void(int, Rect, double)> place = [&](int parent, Rect area,
                                                     double base) {
    auto &list = children[parent + 1];
    if (list.empty())
      return;
    double largest = 0, total = 0;
    for (int i : list)
      largest = std::max(
          largest, std::sqrt(double(std::max<uint64_t>(4096, e[i].bytes))));
    auto weight = [&](int i) {
      double value = std::sqrt(double(std::max<uint64_t>(4096, e[i].bytes)));
      return layout == "map" ? std::max(value, largest / 4) : value;
    };
    for (int i : list)
      total += weight(i);
    std::map<int, Rect> cells;
    if (layout != "disc") {
      auto pending = list;
      // Preserve the selected sibling order; areas still reflect file size.
      Rect remaining = area;
      std::vector<int> row;
      auto nodeArea = [&](int i) {
        return area.w * area.d * weight(i) / total;
      };
      auto worst = [&](const std::vector<int> &items) {
        double sum = 0, smallest = 1e100, biggest = 0;
        for (int i : items) {
          double a = nodeArea(i);
          sum += a;
          smallest = std::min(smallest, a);
          biggest = std::max(biggest, a);
        }
        double side = std::min(remaining.w, remaining.d);
        return std::max(side * side * biggest / (sum * sum),
                        sum * sum / (side * side * smallest));
      };
      auto placeRow = [&] {
        double sum = 0;
        for (int i : row)
          sum += nodeArea(i);
        if (remaining.w >= remaining.d) {
          double width = sum / remaining.d, top = remaining.z;
          for (int i : row) {
            double height = nodeArea(i) / width;
            cells[i] = {remaining.x, top, width, height};
            top += height;
          }
          remaining.x += width;
          remaining.w = std::max(1e-9, remaining.w - width);
        } else {
          double height = sum / remaining.w, left = remaining.x;
          for (int i : row) {
            double width = nodeArea(i) / height;
            cells[i] = {left, remaining.z, width, height};
            left += width;
          }
          remaining.z += height;
          remaining.d = std::max(1e-9, remaining.d - height);
        }
        row.clear();
      };
      for (int i : pending) {
        auto expanded = row;
        expanded.push_back(i);
        if (!row.empty() && worst(expanded) > worst(row))
          placeRow();
        row.push_back(i);
      }
      if (!row.empty())
        placeRow();
    }
    for (int j = 0; j < int(list.size()); ++j) {
      int i = list[j];
      Rect r;
      if (layout == "disc") {
        double angle = 6.283185307 * j / list.size(),
               radius = std::min(area.w, area.d) * .34,
               width = std::min(area.w, area.d) /
                       (2.5 + std::sqrt(double(list.size())) * 1.5);
        r = {area.x + area.w / 2 + std::cos(angle) * radius - width / 2,
             area.z + area.d / 2 + std::sin(angle) * radius - width / 2, width,
             width};
      } else
        r = cells[i];
      double height = e[i].directory
                          ? 1.8
                          : 2 + std::min(15., std::log2(1. + e[i].bytes) * .55);
      e[i].extent = {std::max(.15, r.w * .94), height,
                     std::max(.15, r.d * .94)};
      e[i].center = {r.x + r.w / 2, base + height / 2, r.z + r.d / 2};
      if (e[i].directory)
        place(i, {r.x + r.w * .04, r.z + r.d * .04, r.w * .92, r.d * .68},
              base + height + .5);
    }
  };
  place(-1, {-80, -60, 160, 120}, 0);
  return scene;
}
int pick(const Scene &scene, Vec origin, Vec direction) {
  int selected = -1;
  double nearest = std::numeric_limits<double>::infinity();
  for (int i = 0; i < int(scene.entries.size()); ++i) {
    auto &e = scene.entries[i];
    double near = 0, far = nearest;
    double o[] = {origin.x, origin.y, origin.z},
           d[] = {direction.x, direction.y, direction.z},
           c[] = {e.center.x, e.center.y, e.center.z},
           s[] = {e.extent.x, e.extent.y, e.extent.z};
    bool hit = true;
    for (int n = 0; n < 3; ++n) {
      double lo = c[n] - s[n] / 2, hi = c[n] + s[n] / 2;
      if (std::abs(d[n]) < 1e-10) {
        if (o[n] < lo || o[n] > hi) {
          hit = false;
          break;
        }
      } else {
        double a = (lo - o[n]) / d[n], b = (hi - o[n]) / d[n];
        if (a > b)
          std::swap(a, b);
        near = std::max(near, a);
        far = std::min(far, b);
        if (near > far) {
          hit = false;
          break;
        }
      }
    }
    if (hit && far >= 0 && near < nearest) {
      nearest = near;
      selected = i;
    }
  }
  return selected;
}
Location locationArgument(const std::string &argument) {
  if (argument == "trash:" || argument == "trash:/" || argument == "trash://" ||
      argument == "trash:///") {
    Location result;
    result.trash = true;
    return result;
  }
  if (argument.rfind("archive:", 0) == 0) {
    auto split=argument.find('#');
    auto encoded=argument.substr(8,split==std::string::npos?std::string::npos:split-8);
    gchar *path=g_uri_unescape_string(encoded.c_str(),nullptr);
    gchar *member=split==std::string::npos?g_strdup(""):g_uri_unescape_string(argument.substr(split+1).c_str(),nullptr);
    if(!path || !member){g_free(path);g_free(member);throw std::runtime_error("Invalid archive URL");}
    Location result{path,member,true};g_free(path);g_free(member);
    result.member=safeMember(result.member);result.path=fs::absolute(result.path).lexically_normal();return result;
  }
  if (argument.rfind("file:", 0) == 0) {
    gchar *host=nullptr;GError *error=nullptr;
    gchar *path=g_filename_from_uri(argument.c_str(),&host,&error);
    bool local=!host || !*host || g_ascii_strcasecmp(host,"localhost")==0;
    g_free(host);
    if(!path || !local){g_free(path);if(error)g_error_free(error);throw std::runtime_error("Only valid local file URLs can be opened");}
    Location result{path};g_free(path);return result;
  }
  auto split=argument.find("!/");
  std::error_code ec;
  if(split!=std::string::npos && !fs::exists(argument,ec))return {fs::absolute(argument.substr(0,split)).lexically_normal(),safeMember(argument.substr(split+2)),true};
  return {argument};
}
fs::path materialize(const Location &location, const fs::path &temporary, const ArchiveOptions &options) {
  auto targetMember = safeMember(location.member);
  if (targetMember.empty())
    throw std::runtime_error("Choose an archive file");
  ArchiveReader reader(location.path, options.password.empty() ? location.password : options.password);
  archive_entry *item;
  while (reader.next(item)) {
    std::string member;
    try { member = reader.member(item); }
    catch (const std::exception &) { reader.skip(); continue; }
    if (member != targetMember)
      continue;
    if (archive_entry_filetype(item) != AE_IFREG ||
        archive_entry_hardlink(item) || archive_entry_symlink(item))
      throw std::runtime_error("Archive links cannot be opened");
    if (archive_entry_size(item) > 64 * 1024 * 1024)
      throw std::runtime_error("Archive preview exceeds 64 MiB");
    fs::create_directories(temporary);
    auto target = temporary / fs::path(member).filename();
    int fd = open(target.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
      throw std::runtime_error("Cannot create archive preview");
    uint64_t total = 0;
    char buffer[65536];
    try {
      for (;;) {
        if (options.cancel && options.cancel->load()) throw std::runtime_error("Archive operation cancelled");
        auto n = reader.read(buffer, sizeof(buffer));
        if (n < 0)
          throw std::runtime_error("Cannot read archive member");
        if (!n)
          break;
        total += n;
        if (options.progress) options.progress(total, 1, member);
        if (total > 64 * 1024 * 1024)
          throw std::runtime_error("Archive preview exceeds 64 MiB");
        size_t sent = 0;
        while (sent < size_t(n)) {
          auto w = write(fd, buffer + sent, n - sent);
          if (w <= 0)
            throw std::runtime_error("Cannot write archive preview");
          sent += w;
        }
      }
      close(fd);
      return target;
    } catch (...) {
      close(fd);
      fs::remove(target);
      throw;
    }
  }
  throw std::runtime_error("Archive member disappeared");
}
} // namespace fsv
