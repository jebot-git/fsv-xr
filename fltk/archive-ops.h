// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <archive.h>
#include <archive_entry.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace fsv {
struct ArchivePasswordRequired : std::runtime_error {
	using std::runtime_error::runtime_error;
};
struct ArchiveOptions {
	std::string password;
	std::shared_ptr<std::atomic_bool> cancel;
	std::function<void(uint64_t, size_t, const std::string &)> progress;
	std::vector<std::string>
	    members;  // Empty means everything; folders include descendants.
	uint64_t maxBytes = 0;	// Zero permits large archives; integer overflow
				// is always checked.
	size_t maxEntries = 1000000;
};
struct ArchiveFormat {
	std::string label, extension;
	int format, filter;
};
const std::vector<ArchiveFormat> &archiveFormats();
std::filesystem::path extractionDestination(const std::filesystem::path &);
std::filesystem::path extract(const std::filesystem::path &,
			      const std::filesystem::path &,
			      const ArchiveOptions & = {});
void compress(const std::vector<std::filesystem::path> &,
	      const std::filesystem::path &, const ArchiveOptions & = {});
// Internal reader shared by listing, previews and extraction. Only EOF ends a
// successful read; damaged headers/data and missing passwords remain errors.
struct ArchiveLocale;
class ArchiveReader {
	std::unique_ptr<ArchiveLocale> locale;
	std::filesystem::path source;

       public:
	struct archive *a = nullptr;
	explicit ArchiveReader(const std::filesystem::path &,
			       const std::string &password = {});
	~ArchiveReader();
	ArchiveReader(const ArchiveReader &) = delete;
	ArchiveReader &operator=(const ArchiveReader &) = delete;
	bool next(struct archive_entry *&);
	void skip();
	la_ssize_t read(void *, size_t);
	std::string member(struct archive_entry *) const;
	void check(int result, const char *operation) const;
};
}  // namespace fsv
