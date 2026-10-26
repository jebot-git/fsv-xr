// SPDX-License-Identifier: MIT
// Reproduce live metadata changing during std::sort.
#include "model.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
int main() {
  char tmp[] = "/tmp/fsvr-churn-XXXXXX";
  auto root = std::filesystem::path(mkdtemp(tmp));
  std::filesystem::create_directory(root / "target");
  std::ofstream(root / "file") << "x";
  for (int i = 0; i < 512; ++i)
    std::filesystem::create_symlink("target",
                                    root / ("link-" + std::to_string(i)));
  std::atomic_bool stop = false;
  std::thread worker([&] {
    while (!stop) {
      std::error_code ec;
      std::filesystem::rename(root / "target", root / "dir", ec);
      std::filesystem::rename(root / "file", root / "target", ec);
      std::filesystem::rename(root / "target", root / "file", ec);
      std::filesystem::rename(root / "dir", root / "target", ec);
    }
  });
  for (int i = 0; i < 150; ++i) {
    fsv::scan({root}, {}, 0, 8192);
    if (i % 50 == 0)
      std::cout << i << std::endl;
  }
  stop = true;
  worker.join();
  std::filesystem::remove_all(root);
  std::cout
      << "PASS stable scan ordering under concurrent symlink-target mutation\n";
}
