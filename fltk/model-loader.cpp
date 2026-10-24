// SPDX-License-Identifier: MIT
#include "model-loader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace fsv::gltf {
namespace {
struct Stamp {
  std::filesystem::path path;
  std::filesystem::file_time_type time{};
  uintmax_t size = 0;
  bool exists = false;
  bool operator==(const Stamp &o) const {
    return path == o.path && time == o.time && size == o.size &&
           exists == o.exists;
  }
};
Stamp stamp(const std::filesystem::path &p) {
  Stamp s;
  s.path = p;
  std::error_code error;
  s.time = std::filesystem::last_write_time(p, error);
  if (!error) {
    s.exists = true;
    s.size = std::filesystem::file_size(p, error);
    if (error)
      s.size = 0;
  }
  return s;
}
std::vector<Stamp> stamps(const std::shared_ptr<const Model> &model,
                          const std::filesystem::path &path) {
  std::vector<Stamp> result;
  if (model)
    for (auto &p : model->dependencies)
      result.push_back(stamp(p));
  else if (!path.empty())
    result.push_back(stamp(path));
  return result;
}
} // namespace
struct Loader::State {
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false, pending = false;
  std::filesystem::path path;
  uint64_t generation = 0;
  Snapshot result;
  std::shared_ptr<std::atomic_bool> cancel;
};
Loader::Loader() : state(std::make_shared<State>()) {
  auto shared = state;
  std::thread([shared] {
    std::vector<Stamp> watch;
    auto nextRetry = std::chrono::steady_clock::now();
    for (;;) {
      std::filesystem::path path;
      uint64_t generation = 0;
      std::shared_ptr<std::atomic_bool> cancel;
      {
        std::unique_lock<std::mutex> lock(shared->mutex);
        shared->wake.wait_for(lock, std::chrono::milliseconds(500),
                              [&] { return shared->stop || shared->pending; });
        if (shared->stop)
          return;
        if (!shared->pending) {
          auto model = shared->result.model;
          auto source = shared->path;
          auto previous = watch;
          lock.unlock();
          auto observed = stamps(model, source);
          lock.lock();
          if (shared->stop)
            return;
          if (source != shared->path)
            continue;
          if (observed != previous ||
              (!shared->result.error.empty() &&
               std::chrono::steady_clock::now() >= nextRetry)) {
            shared->pending = !source.empty();
            watch = std::move(observed);
            nextRetry =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
          }
        }
        if (!shared->pending)
          continue;
        path = shared->path;
        generation = shared->generation;
        shared->pending = false;
        if (path.empty()) {
          shared->result = {};
          watch.clear();
          continue;
        }
        cancel = std::make_shared<std::atomic_bool>(false);
        shared->cancel = cancel;
        shared->result.loading = true;
        ++shared->result.revision;
      }
      std::shared_ptr<const Model> model;
      std::string error;
      try {
        model = load(path, cancel);
      } catch (const std::exception &e) {
        error = e.what();
      } catch (...) {
        error = "Unknown glTF loader failure";
      }
      std::shared_ptr<const Model> effective = model;
      if (!effective) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        effective = shared->result.model;
      }
      auto observed = stamps(effective, path);
      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->stop)
          return;
        if (generation != shared->generation || cancel->load())
          continue;
        if (model)
          shared->result.model = std::move(model);
        shared->result.error = std::move(error);
        shared->result.loading = false;
        ++shared->result.revision;
        watch = std::move(observed);
      }
    }
  }).detach();
}
Loader::~Loader() {
  std::lock_guard<std::mutex> lock(state->mutex);
  state->stop = true;
  if (state->cancel)
    state->cancel->store(true);
  state->wake.notify_all();
}
void Loader::open(const std::filesystem::path &path) {
  std::lock_guard<std::mutex> lock(state->mutex);
  if (path == state->path && !state->result.error.size())
    return;
  state->path = path;
  ++state->generation;
  state->pending = true;
  if (state->cancel)
    state->cancel->store(true);
  auto revision = state->result.revision + 1;
  state->result = {};
  state->result.revision = revision;
  state->result.loading = !path.empty();
  state->wake.notify_all();
}
void Loader::clear() { open({}); }
Loader::Snapshot Loader::snapshot() const {
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->result;
}
} // namespace fsv::gltf
