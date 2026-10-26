// SPDX-License-Identifier: LGPL-2.1-or-later
#include "model.h"
#include <gio/gio.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace fsv {
std::string smbUri(const std::string &input) {
  auto text = input;
  if (text.rfind("\\\\", 0) == 0) {
    text = "smb:" + text;
    std::replace(text.begin(), text.end(), '\\', '/');
  }
  if (text.size() > 8192 || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error("Invalid SMB address");
  // GUri normalizes dot segments, including encoded ones. Reject them before
  // parsing so a malformed share URL cannot silently select another share.
  auto authority = text.find("://");
  auto rawPath = authority == std::string::npos ? std::string::npos : text.find('/', authority + 3);
  if (rawPath != std::string::npos) {
    auto end = text.find_first_of("?#", rawPath);
    auto parts = g_strsplit(text.substr(rawPath, end - rawPath).c_str(), "/", -1);
    bool invalid = false;
    for (int i = 0; parts[i]; ++i) {
      auto part = g_uri_unescape_string(parts[i], nullptr);
      invalid |= !part || !strcmp(part, ".") || !strcmp(part, "..");
      g_free(part);
    }
    g_strfreev(parts);
    if (invalid) throw std::runtime_error("Invalid SMB path component");
  }
  GError *error = nullptr;
  GUri *uri = g_uri_parse(text.c_str(), G_URI_FLAGS_ENCODED_PATH, &error);
  if (!uri) { g_clear_error(&error); throw std::runtime_error("Use smb://server/share or \\\\server\\share"); }
  auto owned = std::shared_ptr<GUri>(uri, g_uri_unref);
  const auto host = g_uri_get_host(uri), scheme = g_uri_get_scheme(uri);
  if (!scheme || g_ascii_strcasecmp(scheme, "smb") || !host || !*host ||
      g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
      g_uri_get_port(uri) == 0)
    throw std::runtime_error("Use an SMB server address without credentials, query or fragment");
  std::string path;
  auto parts = g_strsplit(g_uri_get_path(uri), "/", -1);
  for (int i = 0; parts[i]; ++i) {
    if (!*parts[i]) continue;
    auto part = g_uri_unescape_string(parts[i], "/\\");
    if (!part || !g_utf8_validate(part, -1, nullptr) || std::string(part) == "." || std::string(part) == ".." ||
        std::any_of(part, part + (part ? strlen(part) : 0), [](unsigned char c) { return c < 32 || c == 127; })) {
      g_free(part); g_strfreev(parts); throw std::runtime_error("Invalid SMB path component");
    }
    auto encoded = g_uri_escape_string(part, nullptr, false);
    path += "/" + std::string(encoded);
    g_free(encoded); g_free(part);
  }
  g_strfreev(parts);
  auto result = g_uri_join(G_URI_FLAGS_ENCODED_PATH, "smb", nullptr, host,
                           g_uri_get_port(uri), path.empty() ? "/" : path.c_str(), nullptr, nullptr);
  std::string out(result); g_free(result); return out;
}
}
