// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop-files.h"
#include <iostream>
#include <unistd.h>
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() try {
  char directory[] = "/tmp/fsvr-network-test-XXXXXX";
  require(mkdtemp(directory), "Private configuration");
  setenv("HOME", directory, 1); setenv("XDG_CONFIG_HOME", directory, 1);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } clean{directory};
  require(fsv::smbUri("smb://server/share/český 日本語") == "smb://server/share/%C4%8Desk%C3%BD%20%E6%97%A5%E6%9C%AC%E8%AA%9E", "UTF-8 paths");
  require(fsv::smbUri("\\\\server\\share\\folder") == "smb://server/share/folder", "UNC paths");
  require(fsv::smbUri("smb://[::1]:1445/share") == "smb://[::1]:1445/share", "IPv6 and port");
  for (auto value : {"smb://user:secret@host/share", "smb://user@host/share", "smb:///share", "smb://host/share?password=x",
                     "smb://host/share#x", "smb://host/%2e%2e", "smb://host/a%2fb", "smb://host/a%00b", "smb://host/a%0Ab", "https://host/share"}) {
    bool rejected = false; try { fsv::smbUri(value); } catch (...) { rejected = true; }
    require(rejected, value);
  }
  auto location = fsv::locationArgument("smb://server/share/folder");
  require(location.path.empty() && location.display() == "smb://server/share/folder", "SMB URLs never become local relative paths");
  fsv::bookmark(location, "Network share");
  bool found = false;
  for (auto &place : fsv::places()) if (place.name == "Network share") {
    found = true; require(place.location == location && place.location.path.empty(), "Stable bookmark identity");
  }
  require(found && fsv::removeBookmark(location), "Save and remove SMB bookmarks without a connection");
  std::cout << "PASS SMB/UNC/IPv6 paths, credential rejection, traversal boundaries and stable bookmarks\n";
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
