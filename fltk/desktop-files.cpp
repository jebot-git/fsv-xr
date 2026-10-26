// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop-files.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
namespace fsv {
namespace {
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error(s); }
void check(const std::shared_ptr<std::atomic_bool> &cancel) {
  if (cancel && *cancel)
    fail("File operation cancelled");
}
struct Unref {
  template <class T> void operator()(T *p) const {
    if (p)
      g_object_unref(p);
  }
};
template <class T> using Object = std::unique_ptr<T, Unref>;
void error(GError *e, const std::string &fallback) {
  std::string message = e ? e->message : fallback;
  g_clear_error(&e);
  fail(message);
}
bool entryExists(const fs::path &p) {
  std::error_code ec;
  return fs::symlink_status(p, ec).type() != fs::file_type::not_found && !ec;
}
bool within(const fs::path &p, const fs::path &root) {
  auto a = p.begin();
  for (auto &part : root) {
    if (a == p.end() || *a != part)
      return false;
    ++a;
  }
  return true;
}
bool owned(const fs::path &p) {
  struct stat s{};
  return !lstat(p.c_str(), &s) && S_ISDIR(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0022);
}
bool usable(const TrashLocation &t) {
  return owned(t.directory) && owned(t.directory / "files") && owned(t.directory / "info");
}
Entry readTrash(const TrashLocation &t, const fs::path &p) {
  Entry e;
  e.path = p;
  e.name = p.filename();
  struct stat s{};
  if (lstat(p.c_str(), &s))
    fail("Trash item disappeared");
  e.symlink = S_ISLNK(s.st_mode);
  e.directory = S_ISDIR(s.st_mode);
  e.bytes = e.directory ? 0 : std::max<int64_t>(0, s.st_size);
  e.modified = int64_t(s.st_mtim.tv_sec) * 1000000000 + s.st_mtim.tv_nsec;
  e.trash = Entry::Trash{t.directory,
                         t.top,
                         {},
                         t.home,
                         "",
                         uint64_t(s.st_dev),
                         uint64_t(s.st_ino),
                         s.st_ctim.tv_sec,
                         s.st_ctim.tv_nsec};
  auto info = t.directory / "info" / (p.filename().string() + ".trashinfo");
  int fd = open(info.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd >= 0) {
    struct Close {
      int fd;
      ~Close() { close(fd); }
    } closeFd{fd};
    struct stat metadata{};
    if (!fstat(fd, &metadata) && S_ISREG(metadata.st_mode) && metadata.st_size <= 65536) {
      std::string text;
      char buf[4096];
      ssize_t n;
      while ((n = read(fd, buf, sizeof buf)) > 0 && text.size() <= 65536)
        text.append(buf, n);
      std::istringstream input(text);
      std::string line, encoded, deleted;
      bool header = false, havePath = false;
      auto clean = [](std::string &line) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
      };
      if (std::getline(input, line)) {
        clean(line);
        header = line == "[Trash Info]";
      }
      while (header && std::getline(input, line)) {
        clean(line);
        if (!havePath && line.rfind("Path=", 0) == 0) {
          encoded = line.substr(5);
          havePath = true;
        }
        if (deleted.empty() && line.rfind("DeletionDate=", 0) == 0)
          deleted = line.substr(13);
      }
      gchar *decoded = header ? g_uri_unescape_string(encoded.c_str(), nullptr) : nullptr;
      if (decoded) {
        fs::path original = decoded;
        g_free(decoded);
        bool safe = !original.empty() && (t.home ? original.is_absolute() : original.is_relative());
        for (auto &part : original)
          if (part == "..")
            safe = false;
        original = (original.is_absolute() ? original : t.top / original).lexically_normal();
        if (safe && original != "/" && !within(original, t.directory)) {
          e.trash->original = original;
          e.trash->deleted = deleted;
          e.name = original.filename();
        }
      }
    }
  }
  e.type = e.directory ? "Directory" : e.symlink ? "Symbolic link" : "File";
  return e;
}
void validateTrash(const Entry &e) {
  if (!e.trash)
    fail("Select an item in Trash");
  auto &t = *e.trash;
  TrashLocation location{t.store, t.top, t.home};
  if (!usable(location) || e.path.parent_path() != t.store / "files")
    fail("Trash location changed; refresh before trying again");
  auto now = readTrash(location, e.path);
  auto &n = *now.trash;
  if (t.original != n.original || t.deleted != n.deleted || t.device != n.device ||
      t.inode != n.inode || t.changedSeconds != n.changedSeconds ||
      t.changedNanos != n.changedNanos)
    fail("Trash item changed; refresh before trying again");
}
void renameNoReplace(const fs::path &from, const fs::path &to) {
  if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE))
    fail("Cannot move to " + to.string() + ": " + strerror(errno));
}
fs::path bookmarkFile() { return fs::path(g_get_user_config_dir()) / "fsvr/places"; }
std::string readSmall(const fs::path &p) {
  std::error_code ec;
  auto size = fs::file_size(p, ec);
  if (ec || size > 1024 * 1024)
    return {};
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), {}};
}
void writeBookmarks(const std::string &s) {
  auto p = bookmarkFile();
  fs::create_directories(p.parent_path());
  Object<GFile> f(g_file_new_for_path(p.c_str()));
  GError *e = nullptr;
  if (!g_file_replace_contents(f.get(), s.data(), s.size(), nullptr, false, G_FILE_CREATE_PRIVATE,
                               nullptr, nullptr, &e))
    error(e, "Cannot save bookmarks");
}
} // namespace
std::string fileUri(const fs::path &p) {
  auto absolute = fs::absolute(p).lexically_normal();
  GError *e = nullptr;
  auto uri = g_filename_to_uri(absolute.c_str(), nullptr, &e);
  if (!uri)
    error(e, "Invalid file path");
  std::string result = uri;
  g_free(uri);
  return result;
}
fs::path localFile(const std::string &uri) {
  if (uri.find('\0') != std::string::npos)
    fail("Invalid file URL");
  gchar *host = nullptr;
  GError *e = nullptr;
  gchar *p = g_filename_from_uri(uri.c_str(), &host, &e);
  bool local = !host || !*host || !g_ascii_strcasecmp(host, "localhost");
  g_free(host);
  if (!p || !local) {
    g_free(p);
    if (e)
      g_error_free(e);
    fail("Only local file URLs are supported for transfers");
  }
  fs::path result = p;
  g_free(p);
  return result;
}
std::string uriList(const FileList &files) {
  std::string out;
  for (auto &p : files.paths)
    out += fileUri(p) + "\r\n";
  if (out.size() > 1024 * 1024)
    fail("File selection exceeds the clipboard limit");
  return out;
}
FileList parseFiles(const std::string &bytes, const std::string &mime) {
  if (bytes.size() > 1024 * 1024 || bytes.find('\0') != std::string::npos)
    fail("Invalid clipboard file list");
  FileList result;
  std::istringstream input(bytes);
  std::string line;
  std::set<fs::path> seen;
  if (mime == "x-special/gnome-copied-files") {
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line != "cut" && line != "copy")
      fail("Invalid clipboard operation");
    result.cut = line == "cut";
  }
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    fs::path p = line.rfind("file:", 0) == 0              ? localFile(line)
                 : mime == "text/plain" && line[0] == '/' ? fs::path(line)
                                                          : fs::path();
    if (p.empty())
      fail("Clipboard does not contain a local file list");
    p = fs::absolute(p).lexically_normal();
    if (seen.insert(p).second)
      result.paths.push_back(p);
    if (result.paths.size() > 4096)
      fail("Too many files in clipboard");
  }
  return result;
}
std::vector<TrashLocation> trashLocations() {
  std::vector<TrashLocation> out;
  std::set<fs::path> seen;
  auto add = [&](TrashLocation t) {
    t.directory = t.directory.lexically_normal();
    if (usable(t) && seen.insert(t.directory).second)
      out.push_back(t);
  };
  auto data = fs::path(g_get_user_data_dir());
  add({data / "Trash", data, true});
  auto uid = std::to_string(getuid());
  auto mounts = g_unix_mount_entries_get(nullptr);
  for (auto l = mounts; l; l = l->next) {
    auto m = static_cast<GUnixMountEntry *>(l->data);
    fs::path top = g_unix_mount_entry_get_mount_path(m);
    struct stat st{};
    auto shared = top / ".Trash";
    if (!lstat(shared.c_str(), &st) && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX))
      add({shared / uid, top, false});
    add({top / (".Trash-" + uid), top, false});
  }
  g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
  return out;
}
Result scanTrash(const std::vector<TrashLocation> &stores,
                 const std::shared_ptr<std::atomic_bool> &cancel, size_t limit) {
  Result result;
  bool damaged = false;
  for (auto &t : stores) {
    if (!usable(t))
      continue;
    std::error_code ec;
    fs::directory_iterator it(t.directory / "files", ec), end;
    if (ec) {
      result.warning += "Cannot read Trash: " + t.directory.string() + ". ";
      continue;
    }
    while (it != end) {
      check(cancel);
      if (limit && result.entries.size() >= limit) {
        result.warning +=
            "Trash view limited; Empty Trash still applies to all items on available volumes.";
        return result;
      }
      try {
        auto e = readTrash(t, it->path());
        damaged |= e.trash->original.empty();
        result.entries.push_back(std::move(e));
      } catch (const std::exception &) {
        result.warning += "An item disappeared during the scan. ";
      }
      it.increment(ec);
      if (ec) {
        result.warning += "Some Trash items could not be read. ";
        break;
      }
    }
  }
  if (damaged)
    result.warning += "Some Trash information is damaged; these items can be deleted but cannot be "
                      "restored automatically.";
  return result;
}
void moveToTrash(const fs::path &p) {
  Object<GFile> f(g_file_new_for_path(p.c_str()));
  GError *e = nullptr;
  if (!g_file_trash(f.get(), nullptr, &e))
    error(e, "Moving to Trash failed");
}
void restoreTrash(const Entry &e) {
  validateTrash(e);
  auto &t = *e.trash;
  if (t.original.empty())
    fail("Original location is missing or invalid");
  if (!fs::is_directory(t.original.parent_path()))
    fail("Recreate the original parent folder before restoring: " +
         t.original.parent_path().string());
  if (within(fs::weakly_canonical(t.original.parent_path()) / t.original.filename(),
             fs::weakly_canonical(t.store)))
    fail("Cannot restore into a Trash store");
  renameNoReplace(e.path, t.original);
  if (!fs::remove(t.store / "info" / (e.path.filename().string() + ".trashinfo")))
    fail("Item restored; stale Trash information could not be removed");
}
void eraseTrash(const Entry &e) {
  validateTrash(e);
  fs::remove_all(e.path);
  std::error_code ec;
  fs::remove(e.trash->store / "info" / (e.path.filename().string() + ".trashinfo"), ec);
  if (ec)
    fail("Item deleted; stale Trash information could not be removed");
}
void emptyTrash(const std::vector<TrashLocation> &stores,
                const std::shared_ptr<std::atomic_bool> &cancel) {
  auto items = scanTrash(stores, cancel, 0);
  std::string errors = items.warning;
  const std::string damaged = "Some Trash information is damaged; these items can be deleted but "
                              "cannot be restored automatically.";
  if (auto offset = errors.find(damaged); offset != std::string::npos)
    errors.erase(offset, damaged.size());
  for (auto &e : items.entries) {
    check(cancel);
    try {
      eraseTrash(e);
    } catch (const std::exception &x) {
      errors += e.name + ": " + x.what() + "\n";
    }
  }
  if (!errors.empty())
    fail(errors);
}
std::vector<Place> places() {
  std::vector<Place> out;
  std::set<std::string> seen;
  auto add = [&](std::string name, Location location, bool editable = false) {
    if (seen.insert(location.display()).second)
      out.push_back({std::move(name), std::move(location), editable});
  };
  add("Home", {g_get_home_dir()});
  for (int i = 0; i < G_USER_N_DIRECTORIES; ++i)
    if (auto p = g_get_user_special_dir(GUserDirectory(i)))
      if (fs::is_directory(p))
        add(fs::path(p).filename(), {p});
  Location trash;
  trash.trash = true;
  add("Trash", trash);
  add("File System", {"/"});
  auto bookmarks = [&](const fs::path &p, bool editable) {
    std::istringstream input(readSmall(p));
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty())
        continue;
      auto split = line.find(' ');
      auto uri = line.substr(0, split);
      try {
        Location l = uri == "trash:///" ? trash : Location{localFile(uri)};
        auto name =
            split == std::string::npos ? l.path.filename().string() : line.substr(split + 1);
        add(name, l, editable);
      } catch (const std::exception &) {
      }
    }
  };
  bookmarks(bookmarkFile(), true);
  bookmarks(fs::path(g_get_user_config_dir()) / "gtk-3.0/bookmarks", false);
  bookmarks(fs::path(g_get_home_dir()) / ".gtk-bookmarks", false);
  struct Xbel {
    std::string href, title, text;
    bool inTitle = false, hidden = false;
    std::function<void(const std::string &, const std::string &)> add;
  } state;
  state.add = [&](const std::string &uri, const std::string &name) {
    try {
      add(name.empty() ? localFile(uri).filename().string() : name, {localFile(uri)});
    } catch (const std::exception &) {
    }
  };
  GMarkupParser parser{};
  parser.start_element = +[](GMarkupParseContext *, const gchar *name, const gchar **keys,
                             const gchar **values, gpointer data, GError **) {
    auto &s = *static_cast<Xbel *>(data);
    if (!strcmp(name, "bookmark")) {
      s.href.clear();
      s.title.clear();
      s.hidden = false;
      for (int i = 0; keys && keys[i]; ++i)
        if (!strcmp(keys[i], "href"))
          s.href = values[i];
    }
    if (!strcmp(name, "title"))
      s.inTitle = true;
    s.text.clear();
  };
  parser.text =
      +[](GMarkupParseContext *, const gchar *text, gsize size, gpointer data, GError **) {
        auto &s = *static_cast<Xbel *>(data);
        s.text.append(text, size);
        if (s.inTitle)
          s.title.append(text, size);
      };
  parser.end_element = +[](GMarkupParseContext *, const gchar *name, gpointer data, GError **) {
    auto &s = *static_cast<Xbel *>(data);
    if (!strcmp(name, "title"))
      s.inTitle = false;
    if (!strcmp(name, "isHidden") && s.text == "true")
      s.hidden = true;
    if (!strcmp(name, "bookmark") && !s.href.empty() && !s.hidden)
      s.add(s.href, s.title);
  };
  auto xml = readSmall(fs::path(g_get_user_data_dir()) / "user-places.xbel");
  auto ctx = g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &state, nullptr);
  g_markup_parse_context_parse(ctx, xml.data(), xml.size(), nullptr);
  g_markup_parse_context_free(ctx);
  return out;
}
void bookmark(const Location &l, const std::string &name) {
  if (l.archive || l.trash)
    fail("Bookmark a local directory");
  if (!fs::is_directory(l.path))
    fail("Bookmark a directory");
  auto uri = fileUri(l.path);
  std::istringstream input(readSmall(bookmarkFile()));
  std::string line;
  while (std::getline(input, line))
    if (line.substr(0, line.find(' ')) == uri)
      return;
  auto label = name;
  for (char &c : label)
    if (static_cast<unsigned char>(c) < 32)
      c = ' ';
  auto old = readSmall(bookmarkFile());
  if (!old.empty() && old.back() != '\n')
    old += '\n';
  writeBookmarks(old + uri + " " + label + "\n");
}
bool removeBookmark(const Location &l) {
  if (l.trash || l.archive)
    return false;
  auto uri = fileUri(l.path);
  std::istringstream input(readSmall(bookmarkFile()));
  std::string line, out;
  bool removed = false;
  while (std::getline(input, line)) {
    if (line.substr(0, line.find(' ')) == uri)
      removed = true;
    else
      out += line + "\n";
  }
  if (removed)
    writeBookmarks(out);
  return removed;
}

void renameFile(const fs::path &source, const std::string &name) {
  if (name.empty() || name == "." || name == ".." || name.find('\0') != std::string::npos ||
      fs::path(name).filename() != name)
    fail("Enter a single filename");
  if (source.filename() == name)
    return;
  renameNoReplace(source, source.parent_path() / name);
}
std::vector<fs::path> transfer(const FileList &files, const fs::path &destination,
                               const TransferOptions &options) {
  if (!fs::is_directory(destination))
    fail("The destination directory is unavailable");
  auto dest = fs::canonical(destination);
  std::vector<fs::path> sources;
  for (auto p : files.paths) {
    check(options.cancel);
    p = fs::absolute(p).lexically_normal();
    if (p == "/" || p.filename().empty() || !entryExists(p))
      fail("Source is unavailable: " + p.string());
    auto identity = fs::weakly_canonical(p.parent_path()) / p.filename();
    if (fs::is_directory(fs::symlink_status(p)) && within(dest, identity))
      fail("Cannot copy a directory into itself");
    if (std::find(sources.begin(), sources.end(), p) == sources.end())
      sources.push_back(p);
  }
  // A nested selection is already carried by its selected parent directory.
  const auto candidates = sources;
  sources.erase(std::remove_if(sources.begin(), sources.end(),
                               [&](const fs::path &p) {
                                 return std::any_of(
                                     candidates.begin(), candidates.end(), [&](const fs::path &q) {
                                       return p != q && within(p, q) &&
                                              fs::is_directory(fs::symlink_status(q));
                                     });
                               }),
                sources.end());
  std::vector<fs::path> completed;
  auto complete = [&](const fs::path &source) {
    completed.push_back(source);
    if (options.completed)
      options.completed(source);
  };
  uint64_t bytes = 0;
  size_t count = 0;
  Object<GCancellable> cancel(g_cancellable_new());
  std::atomic_bool finished{false};
  std::thread monitor([&] {
    while (!finished) {
      if (options.cancel && *options.cancel)
        g_cancellable_cancel(cancel.get());
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
  });
  struct Join {
    std::atomic_bool &done;
    std::thread &thread;
    ~Join() {
      done = true;
      thread.join();
    }
  } join{finished, monitor};
  for (auto &source : sources) {
    check(options.cancel);
    auto target = dest / source.filename();
    if (files.cut && fs::weakly_canonical(source.parent_path()) == dest) {
      complete(source);
      continue;
    }
    if (entryExists(target)) {
      auto policy = options.collision ? options.collision(target) : Collision::Fail;
      if (policy == Collision::Skip)
        continue;
      if (policy == Collision::Fail)
        fail("Destination exists: " + target.string());
      auto dir = fs::is_directory(fs::symlink_status(source));
      auto stem = dir ? source.filename().string() : source.stem().string(),
           ext = dir ? std::string() : source.extension().string();
      int n = 1;
      do {
        target = dest / (stem + " (copy " + std::to_string(n++) + ")" + ext);
      } while (entryExists(target));
    }
    if (files.cut) {
      if (!syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(),
                   RENAME_NOREPLACE)) {
        complete(source);
        if (options.progress)
          options.progress(bytes, ++count);
        continue;
      }
      if (errno != EXDEV)
        fail("Cannot move " + source.string() + ": " + strerror(errno));
    }
    struct Copied {
      fs::path source, target;
      struct stat before;
    };
    std::vector<Copied> copied;
    bool committed = false, created = false;
    std::function<void(const fs::path &, const fs::path &)> copy = [&](const fs::path &from,
                                                                       const fs::path &to) {
      check(options.cancel);
      struct stat st{};
      if (lstat(from.c_str(), &st))
        fail("Source disappeared: " + from.string());
      if (S_ISDIR(st.st_mode)) {
        if (!fs::create_directory(to))
          fail("Destination exists: " + to.string());
        if (to == target)
          created = true;
        copied.push_back({from, to, st});
        for (auto &child : fs::directory_iterator(from))
          copy(child.path(), to / child.path().filename());
        fs::permissions(to, fs::perms(st.st_mode & 0777));
        fs::last_write_time(to, fs::last_write_time(from));
        if (options.progress)
          options.progress(bytes, ++count);
      } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        Object<GFile> a(g_file_new_for_path(from.c_str())), b(g_file_new_for_path(to.c_str()));
        GError *e = nullptr;
        struct Progress {
          const TransferOptions &options;
          uint64_t before;
          size_t count;
        } progress{options, bytes, count};
        if (!g_file_copy(
                a.get(), b.get(), G_FILE_COPY_NOFOLLOW_SYMLINKS, cancel.get(),
                +[](goffset current, goffset, gpointer data) {
                  auto &p = *static_cast<Progress *>(data);
                  if (p.options.progress)
                    p.options.progress(p.before + uint64_t(std::max<goffset>(0, current)), p.count);
                },
                &progress, &e)) {
          // GIO can leave a partial newly created destination after a failed copy.
          if (!e || !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            std::error_code ec;
            fs::remove(to, ec);
          }
          error(e, "Copy failed");
        }
        if (to == target)
          created = true;
        copied.push_back({from, to, st});
        bytes += S_ISREG(st.st_mode) ? st.st_size : 0;
        if (options.progress)
          options.progress(bytes, ++count);
      } else
        fail("Special files cannot be copied: " + from.string());
    };
    try {
      copy(source, target);
      check(options.cancel);
      if (files.cut) {
        auto unchanged = [](const Copied &c) {
          struct stat now{};
          return !lstat(c.source.c_str(), &now) && now.st_dev == c.before.st_dev &&
                 now.st_ino == c.before.st_ino && now.st_size == c.before.st_size &&
                 now.st_mtim.tv_sec == c.before.st_mtim.tv_sec &&
                 now.st_mtim.tv_nsec == c.before.st_mtim.tv_nsec &&
                 now.st_ctim.tv_sec == c.before.st_ctim.tv_sec &&
                 now.st_ctim.tv_nsec == c.before.st_ctim.tv_nsec;
        };
        for (auto &c : copied)
          if (!unchanged(c))
            fail("Source changed during the move; source was retained");
        committed = true; // Never remove the completed destination after deleting any source file.
        for (auto i = copied.rbegin(); i != copied.rend(); ++i) {
          if (!S_ISDIR(i->before.st_mode) && !unchanged(*i))
            fail("Source changed; the copied destination was retained");
          if (!fs::remove(i->source))
            fail("Copied item could not be removed from its original location");
        }
      }
      committed = true;
      complete(source);
    } catch (...) {
      if (created && !committed) {
        std::error_code ec;
        for (auto &item : copied)
          if (S_ISDIR(item.before.st_mode))
            fs::permissions(item.target, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(target, ec);
      }
      throw;
    }
  }
  return completed;
}
} // namespace fsv
