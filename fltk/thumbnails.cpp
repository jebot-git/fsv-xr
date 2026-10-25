// SPDX-License-Identifier: LGPL-2.1-or-later
#include "thumbnails.h"

#include <gio/gio.h>
#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "thumbnail-decode.h"
namespace fsv {
namespace fs = std::filesystem;
struct Thumbnails::State {
	struct Request {
		fs::path path;
		uint64_t generation;
	};
	std::mutex mutex;
	std::condition_variable wake;
	bool stop = false;
	uint64_t generation = 0;
	size_t bytes = 0;
	std::deque<Request> queue;
	std::map<fs::path, std::shared_ptr<Thumbnail>> images;
	std::set<fs::path> requested;
	std::vector<fs::path> done;
	std::shared_ptr<std::atomic_bool> active;
	std::thread worker;
	State() : worker([this] { run(); }) {}
	~State() {
		{
			std::lock_guard<std::mutex> guard(mutex);
			stop = true;
			if (active) *active = true;
			wake.notify_all();
		}
		worker.join();
	}
	static bool cached(const fs::path &path, fsv_thumbnail &decoded,
			   fsv_thumbnail_cancel cancelled, void *cancel) {
		struct stat st{};
		if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode))
			return false;
		gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
		if (!uri) return false;
		gchar *hash =
		    g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
		auto root = fs::path(g_get_user_cache_dir()) / "thumbnails";
		bool ok = false;
		for (auto size : {"large", "x-large", "normal", "xx-large"}) {
			if (cancelled(cancel)) break;
			auto file = root / size / (std::string(hash) + ".png");
			if (fsv_thumbnail_decode(file.c_str(), 256, 96,
						 &decoded, cancelled, cancel) &&
			    decoded.uri == std::string(uri) &&
			    decoded.mtime == std::to_string(st.st_mtime)) {
				ok = true;
				break;
			}
			fsv_thumbnail_free(&decoded);
		}
		g_free(hash);
		g_free(uri);
		return ok;
	}

	void run() {
		for (;;) {
			Request request;
			std::shared_ptr<std::atomic_bool> cancel;
			{
				std::unique_lock<std::mutex> guard(mutex);
				wake.wait(guard, [this] {
					return stop || !queue.empty();
				});
				if (stop) return;
				request = queue.front();
				queue.pop_front();
				active = cancel =
				    std::make_shared<std::atomic_bool>(false);
			}
			auto image = std::make_shared<Thumbnail>();
			try {
				fsv_thumbnail decoded{};
				auto cancelled = [](void *p) {
					return static_cast<std::atomic_bool *>(
						   p)
						       ->load()
						   ? 1
						   : 0;
				};
				int ok = cached(request.path, decoded,
						cancelled, cancel.get());
				if (!ok && !cancel->load())
					ok = fsv_thumbnail_decode(
					    request.path.c_str(), 256, 96,
					    &decoded, cancelled, cancel.get());
				if (ok) {
					image->width = decoded.width;
					image->height = decoded.height;
					image->rgba.assign(
					    decoded.rgba,
					    decoded.rgba +
						size_t(decoded.width) *
						    decoded.height * 4);
				}
				fsv_thumbnail_free(&decoded);
			} catch (const std::exception &) {
				image = std::make_shared<Thumbnail>();
			}
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (stop) return;
				if (request.generation != generation ||
				    cancel->load())
					continue;
				size_t size = image->rgba.size();
				while (!images.empty() &&
				       (bytes + size > 16 * 1024 * 1024 ||
					images.size() >= 128)) {
					auto first = images.begin();
					bytes -= first->second->rgba.size();
					requested.erase(first->first);
					images.erase(first);
				}
				bytes += size;
				images[request.path] = std::move(image);
				done.push_back(request.path);
				active.reset();
			}
		}
	}
};
Thumbnails::Thumbnails() : state(std::make_unique<State>()) {}
Thumbnails::~Thumbnails() = default;
std::shared_ptr<const Thumbnail> Thumbnails::request(const fs::path &path) {
	std::lock_guard<std::mutex> guard(state->mutex);
	if (auto i = state->images.find(path); i != state->images.end())
		return i->second;
	if (state->queue.size() < 16 && state->requested.insert(path).second) {
		state->queue.push_back({path, state->generation});
		state->wake.notify_one();
	}
	return {};
}
std::vector<fs::path> Thumbnails::completed() {
	std::lock_guard<std::mutex> guard(state->mutex);
	auto done = std::move(state->done);
	state->done.clear();
	return done;
}
void Thumbnails::clear() {
	std::lock_guard<std::mutex> guard(state->mutex);
	++state->generation;
	if (state->active) *state->active = true;
	state->queue.clear();
	state->images.clear();
	state->requested.clear();
	state->done.clear();
	state->bytes = 0;
}
}  // namespace fsv
