// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>

#include "gltf-model.h"
namespace fsv::gltf {
// The worker owns its state, never an FLTK widget. Selection and destruction do
// not wait for filesystem I/O; queued requests replace older unstarted work.
class Loader {
  struct State;
  std::shared_ptr<State> state;

public:
  struct Snapshot {
    std::shared_ptr<const Model> model;
    std::string error;
    bool loading = false;
    uint64_t revision = 0;
  };
  Loader();
  ~Loader();
  Loader(const Loader &) = delete;
  Loader &operator=(const Loader &) = delete;
  void open(const std::filesystem::path &);
  void clear();
  Snapshot snapshot() const;
};
} // namespace fsv::gltf
