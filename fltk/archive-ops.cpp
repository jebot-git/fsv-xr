// SPDX-License-Identifier: LGPL-2.1-or-later
#include "archive-ops.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include "model.h"
namespace fsv {
// libarchive converts UTF-8 entry names through the caller's locale. Set a
// per-thread locale so GUI workers and standalone users behave identically
// without changing the process-global locale under other running threads.
struct ArchiveLocale {
	locale_t value = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr),
		 previous = nullptr;
	ArchiveLocale() {
		if (!value)
			throw std::runtime_error("UTF-8 locale is unavailable");
		previous = uselocale(value);
	}
	~ArchiveLocale() {
		uselocale(previous);
		freelocale(value);
	}
};
namespace {
std::string error(struct archive *a, const char *operation) {
	const char *detail = archive_error_string(a);
	return std::string(operation) + ": " +
	       (detail ? detail : "libarchive operation failed");
}
[[noreturn]] void fail(struct archive *a, const char *operation) {
	auto message = error(a, operation), lower = message;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		       [](unsigned char c) { return std::tolower(c); });
	if (lower.find("passphrase") != std::string::npos ||
	    lower.find("password") != std::string::npos)
		throw ArchivePasswordRequired(
		    "Archive password is required or incorrect");
	throw std::runtime_error(message);
}
void check(struct archive *a, int rc, const char *operation) {
	if (rc != ARCHIVE_OK) fail(a, operation);
}
void cancelled(const ArchiveOptions &o) {
	if (o.cancel && o.cancel->load())
		throw std::runtime_error("Archive operation cancelled");
}
void account(uint64_t &total, uint64_t amount, const ArchiveOptions &o) {
	if (amount > std::numeric_limits<uint64_t>::max() - total ||
	    (o.maxBytes && amount > o.maxBytes - std::min(total, o.maxBytes)))
		throw std::runtime_error("Archive size limit exceeded");
	total += amount;
}
struct Fd {
	int value = -1;
	explicit Fd(int n = -1) : value(n) {
		if (n < 0)
			throw std::runtime_error(
			    std::string("Archive filesystem operation: ") +
			    strerror(errno));
	}
	~Fd() {
		if (value >= 0) close(value);
	}
	Fd(const Fd &) = delete;
	Fd &operator=(const Fd &) = delete;
};
int parentFd(int root, const fs::path &member, bool create) {
	int fd = dup(root);
	if (fd < 0) throw std::runtime_error("Cannot access extraction root");
	try {
		for (const auto &part : member.parent_path()) {
			if (create && mkdirat(fd, part.c_str(), 0700) &&
			    errno != EEXIST)
				throw std::runtime_error(
				    "Cannot create extraction folder");
			int next = openat(
			    fd, part.c_str(),
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (next < 0)
				throw std::runtime_error(
				    "Unsafe or unavailable extraction parent");
			close(fd);
			fd = next;
		}
		return fd;
	} catch (...) {
		close(fd);
		throw;
	}
}
void writeAll(int fd, const char *data, size_t size) {
	while (size) {
		auto n = write(fd, data, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0)
			throw std::runtime_error(
			    "Archive filesystem write failed");
		data += n;
		size -= size_t(n);
	}
}
std::string lowerName(const fs::path &p) {
	auto name = p.filename().string();
	std::transform(name.begin(), name.end(), name.begin(),
		       [](unsigned char c) { return std::tolower(c); });
	return name;
}
bool ends(const std::string &name, const std::string &suffix) {
	return name.size() > suffix.size() &&
	       name.compare(name.size() - suffix.size(), suffix.size(),
			    suffix) == 0;
}
const std::vector<ArchiveFormat> candidates = {
    {"ZIP", ".zip", ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE},
    {"7-Zip", ".7z", ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE},
    {"POSIX tar", ".tar", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_NONE},
    {"Tar + gzip", ".tar.gz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_GZIP},
    {"Tar + bzip2", ".tar.bz2", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_BZIP2},
    {"Tar + xz", ".tar.xz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_XZ},
    {"Tar + Zstandard", ".tar.zst", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_ZSTD},
    {"Tar + LZ4", ".tar.lz4", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_LZ4},
    {"Tar + LZMA", ".tar.lzma", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_LZMA},
    {"Tar + lzop", ".tar.lzo", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_LZOP},
    {"Tar + lzip", ".tar.lz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_LZIP},
    {"Tar + compress", ".tar.z", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
     ARCHIVE_FILTER_COMPRESS},
    {"CPIO (newc)", ".cpio", ARCHIVE_FORMAT_CPIO_SVR4_NOCRC,
     ARCHIVE_FILTER_NONE},
    {"CPIO + gzip", ".cpio.gz", ARCHIVE_FORMAT_CPIO_SVR4_NOCRC,
     ARCHIVE_FILTER_GZIP},
    {"CPIO + xz", ".cpio.xz", ARCHIVE_FORMAT_CPIO_SVR4_NOCRC,
     ARCHIVE_FILTER_XZ},
    {"CPIO + Zstandard", ".cpio.zst", ARCHIVE_FORMAT_CPIO_SVR4_NOCRC,
     ARCHIVE_FILTER_ZSTD},
    {"BSD ar (files only)", ".a", ARCHIVE_FORMAT_AR_BSD, ARCHIVE_FILTER_NONE},
    {"ISO9660 / Rock Ridge", ".iso", ARCHIVE_FORMAT_ISO9660,
     ARCHIVE_FILTER_NONE},
    {"XAR", ".xar", ARCHIVE_FORMAT_XAR, ARCHIVE_FILTER_NONE},
    {"mtree manifest (metadata only)", ".mtree", ARCHIVE_FORMAT_MTREE,
     ARCHIVE_FILTER_NONE},
    {"WARC (files only)", ".warc", ARCHIVE_FORMAT_WARC, ARCHIVE_FILTER_NONE},
    {"Shell archive (write only)", ".shar", ARCHIVE_FORMAT_SHAR_DUMP,
     ARCHIVE_FILTER_NONE},
    {"gzip (one file)", ".gz", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_GZIP},
    {"bzip2 (one file)", ".bz2", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_BZIP2},
    {"xz (one file)", ".xz", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_XZ},
    {"Zstandard (one file)", ".zst", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_ZSTD},
    {"LZ4 (one file)", ".lz4", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_LZ4},
    {"LZMA (one file)", ".lzma", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_LZMA},
    {"lzop (one file)", ".lzo", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_LZOP},
    {"lzip (one file)", ".lz", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_LZIP},
    {"compress (one file)", ".z", ARCHIVE_FORMAT_RAW, ARCHIVE_FILTER_COMPRESS},
};
ArchiveFormat formatFor(const fs::path &destination) {
	auto name = lowerName(destination);
	for (auto alias :
	     {std::pair<const char *, const char *>{".tgz", ".tar.gz"},
	      {".tbz", ".tar.bz2"},
	      {".tbz2", ".tar.bz2"},
	      {".txz", ".tar.xz"},
	      {".tzst", ".tar.zst"},
	      {".ar", ".a"}})
		if (ends(name, alias.first))
			name = "archive" + std::string(alias.second);
	const ArchiveFormat *best = nullptr;
	for (const auto &f : candidates)
		if (ends(name, f.extension) &&
		    (!best || f.extension.size() > best->extension.size()))
			best = &f;
	if (!best)
		throw std::runtime_error(
		    "Unknown output extension; choose a supported archive "
		    "format");
	return *best;
}
int selectFormat(struct archive *a, int format) {
	if (format == ARCHIVE_FORMAT_AR_BSD)
		return archive_write_set_format_ar_bsd(a);
	return archive_write_set_format(a, format);
}
void setFormat(struct archive *a, const ArchiveFormat &f) {
	check(a, selectFormat(a, f.format), "Select archive format");
	check(a, archive_write_add_filter(a, f.filter),
	      "Select compression filter");
	if (f.format == ARCHIVE_FORMAT_ISO9660)
		check(a,
		      archive_write_set_options(
			  a, "iso9660:rockridge=strict,iso9660:iso-level=3"),
		      "Configure ISO metadata and large files");
}
bool selected(const std::string &member, const ArchiveOptions &o) {
	if (o.members.empty()) return true;
	for (const auto &raw : o.members) {
		auto prefix = safeMember(raw);
		if (member == prefix || member.rfind(prefix + "/", 0) == 0)
			return true;
	}
	return false;
}
std::string safeLink(const std::string &member, const char *target) {
	if (!target || !*target || strlen(target) > 4096 ||
	    strchr(target, '\\'))
		throw std::runtime_error("Invalid archive link");
	fs::path path(target);
	if (path.is_absolute() || path.has_root_name())
		throw std::runtime_error("Absolute archive link blocked");
	auto resolved =
	    (fs::path(member).parent_path() / path).lexically_normal();
	safeMember(resolved.generic_string());
	return path.generic_string();
}
void metadata(int fd, mode_t mode, time_t mtime, long nanos) {
	if (fchmod(fd, mode & 0777))
		throw std::runtime_error("Cannot restore archive permissions");
	timespec times[2] = {{0, UTIME_OMIT}, {mtime, nanos}};
	if (futimens(fd, times))
		throw std::runtime_error("Cannot restore archive timestamp");
}
}  // namespace
ArchiveReader::ArchiveReader(const fs::path &path, const std::string &password)
    : locale(std::make_unique<ArchiveLocale>()),
      source(path),
      a(archive_read_new()) {
	if (!a) throw std::runtime_error("Cannot allocate archive reader");
	try {
		if (archive_version_number() < 3008010)
			throw std::runtime_error(
			    "Archive operations require libarchive 3.8.10 or "
			    "later");
		archive_read_support_filter_all(a);
		archive_read_support_format_all(a);
		archive_read_support_format_raw(a);
		if (!password.empty())
			check(archive_read_add_passphrase(a, password.c_str()),
			      "Set archive password");
		check(archive_read_open_filename(a, path.c_str(), 65536),
		      "Open archive");
	} catch (...) {
		archive_read_free(a);
		a = nullptr;
		throw;
	}
}
ArchiveReader::~ArchiveReader() {
	if (a) archive_read_free(a);
}
void ArchiveReader::check(int result, const char *operation) const {
	if (result != ARCHIVE_OK) fail(a, operation);
}
bool ArchiveReader::next(struct archive_entry *&entry) {
	int rc = archive_read_next_header(a, &entry);
	if (rc == ARCHIVE_EOF) return false;
	check(rc, "Read archive header");
	if (archive_format(a) == ARCHIVE_FORMAT_RAW &&
	    archive_filter_code(a, 0) == ARCHIVE_FILTER_NONE)
		throw std::runtime_error("Not an archive or compressed stream");
	return true;
}
void ArchiveReader::skip() {
	check(archive_read_data_skip(a), "Skip archive member");
}
la_ssize_t ArchiveReader::read(void *buffer, size_t size) {
	auto n = archive_read_data(a, buffer, size);
	if (n < 0) fail(a, "Read archive data");
	return n;
}
std::string ArchiveReader::member(struct archive_entry *entry) const {
	if (archive_format(a) == ARCHIVE_FORMAT_RAW) {
		auto name = source.filename().string();
		auto lower = lowerName(source);
		for (const auto &f : candidates)
			if (f.format == ARCHIVE_FORMAT_RAW &&
			    ends(lower, f.extension))
				return safeMember(name.substr(
				    0, name.size() - f.extension.size()));
		return safeMember(source.stem().string() + ".unpacked");
	}
	const char *name = archive_entry_pathname_utf8(entry);
	if (!name) name = archive_entry_pathname(entry);
	return safeMember(name ? name : "");
}
const std::vector<ArchiveFormat> &archiveFormats() {
	static const auto available = [] {
		std::vector<ArchiveFormat> result;
		for (const auto &f : candidates) {
			auto a = archive_write_new();
			if (!a) continue;
			if (selectFormat(a, f.format) == ARCHIVE_OK &&
			    archive_write_add_filter(a, f.filter) == ARCHIVE_OK)
				result.push_back(f);
			archive_write_free(a);
		}
		return result;
	}();
	return available;
}
bool archiveSupported(const fs::path &path) {
	auto name = lowerName(path);
	for (const auto &f : candidates)
		if (ends(name, f.extension) &&
		    (f.format & ARCHIVE_FORMAT_BASE_MASK) !=
			ARCHIVE_FORMAT_SHAR)
			return true;
	for (auto ext :
	     {".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".rar", ".cab", ".lha",
	      ".lzh", ".deb", ".rpm", ".ar", ".zstd"})
		if (ends(name, ext)) return true;
	try {
		// Documents such as DOCX/ODT/EPUB are ZIP containers but keep
		// their own application associations when opened from the file
		// browser.
		gboolean unsure = false;
		char *mime = g_content_type_guess(path.filename().c_str(),
						  nullptr, 0, &unsure);
		bool generic = unsure || !mime ||
			       g_str_equal(mime, "application/octet-stream");
		bool archiveMime =
		    mime && (g_str_equal(mime, "application/zip") ||
			     g_str_equal(mime, "application/x-tar") ||
			     g_str_equal(mime, "application/x-cpio"));
		g_free(mime);
		if (!generic && !archiveMime) return false;
		if (!fs::is_regular_file(path) || !fs::file_size(path))
			return false;
		ArchiveReader reader(path);
		archive_entry *entry = nullptr;
		return reader.next(entry) &&
		       archive_format(reader.a) != ARCHIVE_FORMAT_EMPTY;
	} catch (const ArchivePasswordRequired &) {
		return true;
	} catch (...) {
		return false;
	}
}
fs::path extractionDestination(const fs::path &source) {
	auto name = source.filename().string(), lower = lowerName(source);
	size_t suffix = 0;
	for (const auto &f : candidates)
		if (ends(lower, f.extension))
			suffix = std::max(suffix, f.extension.size());
	for (auto ext : {".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".rar",
			 ".cab", ".lha", ".lzh", ".deb", ".rpm", ".ar"})
		if (ends(lower, ext)) suffix = std::max(suffix, strlen(ext));
	if (!suffix) suffix = source.extension().string().size();
	return source.parent_path() /
	       (name.substr(0, name.size() - suffix) + ".extracted");
}
fs::path extract(const fs::path &source, const fs::path &destination,
		 const ArchiveOptions &o) {
	cancelled(o);
	if (!fs::create_directory(destination))
		throw std::runtime_error(
		    "Extraction destination already exists");
	chmod(destination.c_str(), 0700);
	try {
		Fd root(open(destination.c_str(),
			     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
		ArchiveReader reader(source, o.password);
		archive_entry *entry = nullptr;
		struct Link {
			std::string member, target;
		};
		struct Dir {
			std::string member;
			mode_t mode;
			time_t mtime;
			long nanos;
		};
		std::vector<Link> hardlinks;
		std::vector<Dir> directories;
		uint64_t total = 0;
		size_t count = 0;
		while (reader.next(entry)) {
			if (archive_format(reader.a) == ARCHIVE_FORMAT_MTREE)
				throw std::runtime_error(
				    "An mtree manifest contains metadata, not "
				    "archived file contents");
			cancelled(o);
			if (++count > o.maxEntries)
				throw std::runtime_error(
				    "Archive entry limit exceeded");
			auto member = reader.member(entry);
			if (member.empty() || !selected(member, o)) {
				reader.skip();
				continue;
			}
			auto type = archive_entry_filetype(entry);
			Fd parent(parentFd(root.value, member, true));
			auto leaf = fs::path(member).filename().string();
			if (const char *target =
				archive_entry_hardlink(entry)) {
				hardlinks.push_back(
				    {member, safeMember(target)});
				reader.skip();
			} else if (type == AE_IFLNK) {
				auto target = safeLink(
				    member, archive_entry_symlink(entry));
				if (symlinkat(target.c_str(), parent.value,
					      leaf.c_str()))
					throw std::runtime_error(
					    "Cannot create archive symbolic "
					    "link; overwrites are refused");
				reader.skip();
			} else if (type == AE_IFDIR) {
				if (mkdirat(parent.value, leaf.c_str(), 0700) &&
				    errno != EEXIST)
					throw std::runtime_error(
					    "Cannot create archive directory");
				Fd dir(openat(parent.value, leaf.c_str(),
					      O_RDONLY | O_DIRECTORY |
						  O_NOFOLLOW | O_CLOEXEC));
				directories.push_back(
				    {member, archive_entry_perm(entry),
				     archive_entry_mtime(entry),
				     archive_entry_mtime_nsec(entry)});
				reader.skip();
			} else if (type == AE_IFREG) {
				Fd output(openat(parent.value, leaf.c_str(),
						 O_WRONLY | O_CREAT | O_EXCL |
						     O_NOFOLLOW | O_CLOEXEC,
						 0600));
				if (o.maxBytes &&
				    archive_entry_size(entry) > 0 &&
				    uint64_t(archive_entry_size(entry)) >
					o.maxBytes -
					    std::min(total, o.maxBytes))
					throw std::runtime_error(
					    "Archive size limit exceeded");
				char buffer[65536];
				for (;;) {
					cancelled(o);
					auto n =
					    reader.read(buffer, sizeof(buffer));
					if (!n) break;
					account(total, uint64_t(n), o);
					writeAll(output.value, buffer,
						 size_t(n));
					if (o.progress)
						o.progress(total, count,
							   member);
				}
				metadata(output.value,
					 archive_entry_perm(entry),
					 archive_entry_mtime(entry),
					 archive_entry_mtime_nsec(entry));
			} else
				throw std::runtime_error(
				    "Archive contains a device, socket or "
				    "other unsupported special file");
			if (o.progress) o.progress(total, count, member);
		}
		std::map<std::string, std::vector<std::string>> dependents;
		for (const auto &link : hardlinks)
			dependents[link.target].push_back(link.member);
		std::vector<std::string> ready;
		for (const auto &[target, links] : dependents) {
			(void)links;
			Fd parent(parentFd(root.value, target, false));
			struct stat st{};
			if (!fstatat(parent.value,
				     fs::path(target).filename().c_str(), &st,
				     AT_SYMLINK_NOFOLLOW) &&
			    S_ISREG(st.st_mode))
				ready.push_back(target);
		}
		size_t linked = 0;
		for (size_t n = 0; n < ready.size(); ++n) {
			cancelled(o);
			auto found = dependents.find(ready[n]);
			if (found == dependents.end()) continue;
			Fd targetParent(parentFd(root.value, ready[n], false));
			auto target = fs::path(ready[n]).filename().string();
			for (const auto &member : found->second) {
				cancelled(o);
				Fd parent(parentFd(root.value, member, false));
				if (linkat(targetParent.value, target.c_str(),
					   parent.value,
					   fs::path(member).filename().c_str(),
					   0))
					throw std::runtime_error(
					    "Cannot create archive hard link");
				ready.push_back(member);
				++linked;
			}
			dependents.erase(found);
		}
		if (linked != hardlinks.size())
			throw std::runtime_error(
			    "Archive hard links contain a cycle or refer to "
			    "files outside the extracted selection");

		std::sort(directories.begin(), directories.end(),
			  [](const auto &a, const auto &b) {
				  return a.member.size() > b.member.size();
			  });
		for (const auto &dir : directories) {
			Fd parent(parentFd(root.value, dir.member, false));
			Fd fd(openat(
			    parent.value,
			    fs::path(dir.member).filename().c_str(),
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
			metadata(fd.value, dir.mode, dir.mtime, dir.nanos);
		}
		return destination;
	} catch (...) {
		std::error_code ec;
		fs::remove_all(destination, ec);
		throw;
	}
}
fs::path extract(const fs::path &source) {
	return extract(source, extractionDestination(source));
}
void compress(const std::vector<fs::path> &input, const fs::path &destination,
	      const ArchiveOptions &o) {
	ArchiveLocale locale;
	if (archive_version_number() < 3008010)
		throw std::runtime_error(
		    "Archive operations require libarchive 3.8.10 or later");
	cancelled(o);
	if (input.empty())
		throw std::runtime_error("Choose files or a folder to archive");
	auto format = formatFor(destination);
	if (!o.password.empty() && format.format != ARCHIVE_FORMAT_ZIP)
		throw std::runtime_error(
		    "Password-protected creation is supported for ZIP "
		    "archives");
	std::vector<fs::path> sources;
	auto base =
	    fs::absolute(input.front()).lexically_normal().parent_path();
	for (auto path : input) {
		path = fs::absolute(path).lexically_normal();
		if (path.parent_path() != base)
			throw std::runtime_error(
			    "Archive sources must share a parent folder");
		if (std::find(sources.begin(), sources.end(), path) !=
		    sources.end())
			continue;
		if (fs::is_directory(fs::symlink_status(path))) {
			auto relative =
			    fs::weakly_canonical(destination)
				.lexically_relative(fs::canonical(path));
			if (!relative.empty() && *relative.begin() != "..")
				throw std::runtime_error(
				    "Archive cannot be written inside its "
				    "source folder");
		}
		sources.push_back(path);
	}
	if (format.format == ARCHIVE_FORMAT_RAW &&
	    (sources.size() != 1 ||
	     !fs::is_regular_file(fs::symlink_status(sources.front()))))
		throw std::runtime_error(
		    "A compressed stream accepts exactly one regular file; "
		    "choose a tar format for folders");
	auto writer = archive_write_new();
	if (!writer) throw std::runtime_error("Cannot allocate archive writer");
	int fd = -1;
	try {
		setFormat(writer, format);
		if (!o.password.empty()) {
			check(writer,
			      archive_write_set_passphrase(writer,
							   o.password.c_str()),
			      "Set ZIP password");
			check(writer,
			      archive_write_set_options(
				  writer, "zip:encryption=aes256"),
			      "Enable ZIP encryption");
		}
		fd = open(destination.c_str(),
			  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			  0600);
		if (fd < 0)
			throw std::runtime_error(
			    "Archive destination exists or cannot be created");
		check(writer, archive_write_open_fd(writer, fd),
		      "Open archive writer");
		std::vector<fs::path> queue = sources;
		uint64_t total = 0;
		size_t count = 0;
		for (size_t i = 0; i < queue.size(); ++i) {
			cancelled(o);
			auto path = queue[i];
			if (++count > o.maxEntries)
				throw std::runtime_error(
				    "Archive entry limit exceeded");
			struct stat st{};
			if (lstat(path.c_str(), &st))
				throw std::runtime_error(
				    "Cannot read archive source metadata");
			auto member = safeMember(
			    path.lexically_relative(base).generic_string());
			if ((format.format & ARCHIVE_FORMAT_BASE_MASK) ==
				ARCHIVE_FORMAT_AR &&
			    !S_ISREG(st.st_mode))
				throw std::runtime_error(
				    "ar archives accept regular files, not "
				    "folders or links");
			std::unique_ptr<archive_entry,
					decltype(&archive_entry_free)>
			    ownedEntry(archive_entry_new(), archive_entry_free);
			auto entry = ownedEntry.get();
			if (!entry)
				throw std::runtime_error(
				    "Cannot allocate archive entry");
			archive_entry_copy_stat(entry, &st);
			archive_entry_set_pathname_utf8(entry, member.c_str());
			if (S_ISDIR(st.st_mode))
				archive_entry_set_size(entry, 0);
			if (S_ISLNK(st.st_mode)) {
				auto target =
				    fs::read_symlink(path).generic_string();
				safeLink(member, target.c_str());
				archive_entry_set_symlink(entry,
							  target.c_str());
				archive_entry_set_size(entry, 0);
			} else if (!S_ISREG(st.st_mode) &&
				   !S_ISDIR(st.st_mode)) {
				throw std::runtime_error(
				    "Source contains an unsupported special "
				    "file");
			}
			auto rc = archive_write_header(writer, entry);
			check(writer, rc, "Write archive header");
			if (S_ISDIR(st.st_mode)) {
				for (const auto &child :
				     fs::directory_iterator(path))
					queue.push_back(child.path());
			} else if (S_ISREG(st.st_mode)) {
				Fd file(open(path.c_str(), O_RDONLY |
							       O_NOFOLLOW |
							       O_CLOEXEC));
				struct stat current{};
				if (fstat(file.value, &current) ||
				    current.st_dev != st.st_dev ||
				    current.st_ino != st.st_ino ||
				    current.st_size != st.st_size)
					throw std::runtime_error(
					    "Archive source changed during "
					    "creation");
				uint64_t remaining = uint64_t(st.st_size);
				char buffer[65536];
				while (remaining) {
					cancelled(o);
					auto n = ::read(
					    file.value, buffer,
					    std::min<uint64_t>(remaining,
							       sizeof(buffer)));
					if (n < 0 && errno == EINTR) continue;
					if (n <= 0)
						throw std::runtime_error(
						    "Archive source read "
						    "failed or file was "
						    "truncated");
					account(total, uint64_t(n), o);
					remaining -= uint64_t(n);
					size_t sent = 0;
					while (sent < size_t(n)) {
						auto w = archive_write_data(
						    writer, buffer + sent,
						    size_t(n) - sent);
						if (w <= 0)
							fail(writer,
							     "Write archive "
							     "data");
						sent += size_t(w);
					}
					if (o.progress)
						o.progress(total, count,
							   member);
				}
			}
			check(writer, archive_write_finish_entry(writer),
			      "Finish archive entry");
			if (o.progress) o.progress(total, count, member);
		}
		check(writer, archive_write_close(writer), "Finalize archive");
		int released = archive_write_free(writer);
		writer = nullptr;
		if (released != ARCHIVE_OK)
			throw std::runtime_error(
			    "Cannot finalize archive writer");
		if (fsync(fd))
			throw std::runtime_error(
			    "Cannot synchronize archive output");
		close(fd);
		fd = -1;
	} catch (...) {
		if (writer) archive_write_free(writer);
		if (fd >= 0) {
			close(fd);
			std::error_code ec;
			fs::remove(destination, ec);
		}
		throw;
	}
}
void compress(const fs::path &source, const fs::path &destination) {
	compress(std::vector<fs::path>{source}, destination);
}
}  // namespace fsv
