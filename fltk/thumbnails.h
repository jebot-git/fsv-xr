// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <filesystem>
#include <memory>
#include <vector>
namespace fsv {
struct Thumbnail {
	int width = 0, height = 0;
	std::vector<unsigned char> rgba;
};
// One decoder worker, 16 queued requests and a 16 MiB CPU image cache per view.
// Changing folders cancels queued/in-flight work and discards stale results.
class Thumbnails {
	struct State;
	std::unique_ptr<State> state;

       public:
	Thumbnails();
	~Thumbnails();
	std::shared_ptr<const Thumbnail> request(const std::filesystem::path &);
	std::vector<std::filesystem::path> completed();
	void clear();
};
}  // namespace fsv
