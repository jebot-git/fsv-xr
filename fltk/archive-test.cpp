// SPDX-License-Identifier: LGPL-2.1-or-later
#include <archive.h>
#include <archive_entry.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>

#include "model.h"
using namespace fsv;
static void require(bool v, const std::string &s) {
	if (!v) throw std::runtime_error(s);
}
static std::string contents(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f),
		std::istreambuf_iterator<char>()};
}
static void refused(const std::function<void()> &fn, const std::string &why) {
	bool failed = false;
	try {
		fn();
	} catch (const std::exception &) {
		failed = true;
	}
	require(failed, why);
}
static void custom(
    const fs::path &file,
    const std::vector<std::tuple<std::string, int, std::string>> &entries) {
	auto a = archive_write_new();
	archive_write_set_format_pax_restricted(a);
	require(archive_write_open_filename(a, file.c_str()) == ARCHIVE_OK,
		"fixture writer");
	for (const auto &[name, type, target] : entries) {
		auto e = archive_entry_new();
		archive_entry_set_pathname(e, name.c_str());
		archive_entry_set_filetype(e, type);
		archive_entry_set_perm(e, type == AE_IFDIR ? 0755 : 0644);
		archive_entry_set_size(
		    e, type == AE_IFREG && target.empty() ? 1 : 0);
		if (type == AE_IFLNK)
			archive_entry_set_symlink(e, target.c_str());
		else if (!target.empty())
			archive_entry_set_hardlink(e, target.c_str());
		require(archive_write_header(a, e) == ARCHIVE_OK,
			"fixture header");
		if (type == AE_IFREG && target.empty())
			archive_write_data(a, "x", 1);
		archive_entry_free(e);
	}
	require(archive_write_close(a) == ARCHIVE_OK, "fixture close");
	archive_write_free(a);
}
int main() try {
	char temp[] = "/tmp/fsvr-archives-XXXXXX";
	require(mkdtemp(temp), "temporary");
	fs::path root = temp;
	struct Cleanup {
		fs::path p;
		~Cleanup() {
			if (std::uncaught_exceptions()) {
				std::cerr << "Fixture retained: " << p << "\n";
				return;
			}
			std::error_code ec;
			fs::remove_all(p, ec);
		}
	} cleanup{root};
	fs::create_directories(root / "source/nested");
	std::string payload;
	for (int i = 0; i < 200000; ++i)
		payload += char((i * 13 + i / 97) % 251);
	{
		std::ofstream f(root / "source/nested/příloha.bin",
				std::ios::binary);
		f << payload;
	}
	std::ofstream(root / "source/run.sh") << "#!/bin/sh\nexit 0\n";
	chmod((root / "source/run.sh").c_str(), 0750);
	auto token = std::make_shared<std::atomic_bool>(false);
	size_t tested = 0;
	for (const auto &format : archiveFormats()) {
		auto dir = root / std::to_string(tested++);
		fs::create_directory(dir);
		auto out = dir / ("fixture" + format.extension);
		bool fileOnly = format.format == ARCHIVE_FORMAT_RAW ||
				(format.format & ARCHIVE_FORMAT_BASE_MASK) ==
				    ARCHIVE_FORMAT_AR ||
				format.format == ARCHIVE_FORMAT_WARC;
		auto source = fileOnly ? root / "source/nested/příloha.bin"
				       : root / "source";
		compress(source, out);
		require(fs::file_size(out) > 0, "created " + format.label);
		if (format.format == ARCHIVE_FORMAT_SHAR_DUMP ||
		    format.format == ARCHIVE_FORMAT_MTREE) {
			std::cout << "PASS writer " << format.label << '\n';
			continue;
		}
		auto list = scan({out, "", true}, token);
		require(!list.entries.empty(), "browse " + format.label);
		auto dest = extract(out);
		auto file = format.format == ARCHIVE_FORMAT_RAW
				? dest / "fixture"
			    : fileOnly ? dest / "příloha.bin"
				       : dest / "source/nested/příloha.bin";
		require(contents(file) == payload,
			"byte-exact round-trip " + format.label);
		if (!fileOnly) {
			struct stat st{};
			require(!stat((dest / "source/run.sh").c_str(), &st) &&
				    (st.st_mode & 0777) == 0750,
				"executable mode preserved " + format.label +
				    " (actual decimal mode " +
				    std::to_string(st.st_mode & 0777) + ")");
		}
		std::cout << "PASS round-trip " << format.label << '\n';
	}
	require(tested >= 20, "broad writer/filter coverage");
	for (const char *name :
	     {"rar-v4.rar", "rar-v5.rar", "cab.cab", "lha.lzh"}) {
		auto source = fs::path(FSVR_ARCHIVE_FIXTURES) / name;
		require(archiveSupported(source),
			"read-only format recognition");
		require(!scan({source, "", true}, token).entries.empty(),
			"read-only format listing");
		auto dest = root / (std::string(name) + ".extracted");
		extract(source, dest);
		if (std::string(name) == "rar-v4.rar")
			require(contents(dest / "test.txt") ==
				    "test text document\r\n",
				"RAR 4 payload");
		if (std::string(name) == "rar-v5.rar")
			require(contents(dest / "helloworld.txt") ==
				    "hello libarchive test suite!\n",
				"RAR 5 payload");
		std::cout << "PASS read-only format " << name << '\n';
	}

	auto encrypted = root / "encrypted.zip";
	ArchiveOptions secret;
	secret.password = "fixture password ü";
	compress(std::vector<fs::path>{root / "source"}, encrypted, secret);
	bool password = false;
	try {
		extract(encrypted, root / "encrypted-out");
	} catch (const ArchivePasswordRequired &) {
		password = true;
	}
	require(password && !fs::exists(root / "encrypted-out"),
		"password required and partial output removed");
	ArchiveOptions wrong;
	wrong.password = "wrong";
	password = false;
	try {
		extract(encrypted, root / "wrong-out", wrong);
	} catch (const ArchivePasswordRequired &) {
		password = true;
	}
	require(password && !fs::exists(root / "wrong-out"), "wrong password");
	auto output = extract(encrypted, root / "secret-out", secret);
	require(contents(output / "source/nested/příloha.bin") == payload,
		"AES ZIP round trip");
	auto preview =
	    materialize({encrypted, "source/nested/příloha.bin", true},
			root / "preview", secret);
	require(contents(preview) == payload, "encrypted member preview");
	auto zip = root / "safe.zip";
	compress(root / "source", zip);
	fs::copy_file(zip, root / "renamed.blob");
	require(archiveSupported(root / "renamed.blob"),
		"content detection independent of extension");
	require(!archiveSupported(root / "source/run.sh"),
		"ordinary files are not raw archives");
	fs::copy_file(zip, root / "document.docx");
	require(!archiveSupported(root / "document.docx"),
		"ZIP documents retain their own default application");
	std::ofstream(root / "plain.blob") << "ordinary data";
	require(!archiveSupported(root / "plain.blob"),
		"raw format needs a real compression filter");
	ArchiveOptions selection;
	selection.members = {"source/nested"};
	extract(zip, root / "selected", selection);
	require(
	    contents(root / "selected/source/nested/příloha.bin") == payload &&
		!fs::exists(root / "selected/source/run.sh"),
	    "selected subtree extraction");
	ArchiveOptions limits;
	limits.maxBytes = 100;
	refused([&] { extract(zip, root / "limited", limits); }, "byte limit");
	require(!fs::exists(root / "limited"), "limit rollback");
	auto cancel = std::make_shared<std::atomic_bool>(false);
	ArchiveOptions cancelling;
	cancelling.cancel = cancel;
	cancelling.progress = [&](auto, auto, const auto &) { *cancel = true; };
	refused([&] { extract(zip, root / "cancelled", cancelling); },
		"extraction cancellation");
	require(!fs::exists(root / "cancelled"), "cancel extraction rollback");
	*cancel = false;
	refused(
	    [&] {
		    compress(std::vector<fs::path>{root / "source"},
			     root / "cancelled.zip", cancelling);
	    },
	    "writer cancellation");
	require(!fs::exists(root / "cancelled.zip"),
		"cancel creation rollback");
	std::ofstream(root / "existing.zip") << "keep";
	refused([&] { compress(root / "source", root / "existing.zip"); },
		"overwrite archive");
	require(contents(root / "existing.zip") == "keep",
		"existing output kept");
	refused([&] { compress(root / "source", root / "source/inside.zip"); },
		"recursive output");
	refused([&] { compress(root / "source", root / "unknown.output"); },
		"unknown writer extension");
	refused([&] { compress(root / "source", root / "folder.gz"); },
		"single stream needs one file");
	auto data = contents(zip);
	data.resize(data.size() / 2);
	{
		std::ofstream f(root / "broken.zip", std::ios::binary);
		f << data;
	}
	refused([&] { extract(root / "broken.zip"); },
		"truncated archive cannot succeed");
	require(!fs::exists(root / "broken.extracted"),
		"damaged archive rollback");
	for (const auto &name : {"../../outside", "/tmp/fsvr-outside",
				 "C:/outside", "a/../../../outside"}) {
		custom(root / "bad.tar", {{name, AE_IFREG, ""}});
		refused([&] { extract(root / "bad.tar"); }, "traversal");
		require(!fs::exists(root / "bad.extracted"),
			"traversal rollback");
	}
	custom(root / "links.tar", {{"folder", AE_IFDIR, ""},
				    {"folder/data", AE_IFREG, ""},
				    {"folder/symbolic", AE_IFLNK, "data"},
				    {"folder/hard", AE_IFREG, "folder/data"}});
	auto links = extract(root / "links.tar");
	require(fs::is_symlink(links / "folder/symbolic") &&
		    contents(links / "folder/symbolic") == "x",
		"safe symlink");
	struct stat a{}, b{};
	stat((links / "folder/data").c_str(), &a);
	stat((links / "folder/hard").c_str(), &b);
	require(a.st_ino == b.st_ino, "hard link inode");

	custom(
	    root / "forward-links.tar",
	    {{"a", AE_IFREG, "b"}, {"b", AE_IFREG, "c"}, {"c", AE_IFREG, ""}});
	auto forward = extract(root / "forward-links.tar");
	require(contents(forward / "a") == "x" &&
		    fs::equivalent(forward / "a", forward / "c"),
		"forward and chained hard links");
	custom(root / "cyclic-links.tar",
	       {{"a", AE_IFREG, "b"}, {"b", AE_IFREG, "a"}});
	refused([&] { extract(root / "cyclic-links.tar"); },
		"cyclic hard links");
	for (auto target : {"../../outside", "/tmp/outside"}) {
		custom(root / "bad-link.tar",
		       {{"folder/link", AE_IFLNK, target}});
		refused([&] { extract(root / "bad-link.tar"); },
			"escaping symbolic link");
	}
	custom(root / "parent-link.tar",
	       {{"folder", AE_IFLNK, "inside"}, {"folder/data", AE_IFREG, ""}});
	refused([&] { extract(root / "parent-link.tar"); },
		"symlink parent traversal");
	gchar *uri = g_filename_to_uri(
	    (root / "name with spaces $ ü.zip").c_str(), nullptr, nullptr);
	require(uri, "URI");
	require(locationArgument(uri).path == root / "name with spaces $ ü.zip",
		"literal file URL");
	g_free(uri);
	std::cout << "PASS passwords, selection, limits, cancellation, "
		     "corruption, safe links, traversal and file URLs\n";
} catch (const std::exception &e) {
	std::cerr << e.what() << '\n';
	return 1;
}
