/**
 * @file        system/guest_path.cpp
 * @brief       Guest path normalization for Xbox 360 VFS paths
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/guest_path.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <cctype>

#include <rex/string/utf8.h>

namespace rex::system {

std::string NormalizeGuestPath(std::string_view path) {
  // Manifest / config consumers expect POSIX-style guest paths: forward
  // slashes, no device prefix, lowercase. The runtime VFS tolerates either
  // separator, so converting backslashes here keeps writers (TOML) happy.
  std::string result = rex::string::utf8_fix_path_separators(path, U'/');

  auto colon = result.find(':');
  if (colon != std::string::npos && colon + 1 < result.size() && result[colon + 1] == '/') {
    result.erase(0, colon + 2);
  }

  result = rex::string::utf8_canonicalize_path(result, U'/');

  while (!result.empty() && result.front() == '/') {
    result.erase(result.begin());
  }

  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // renut-ultimate (2026-11-20, port 204): sims3-nx (github.com/Zumoly/sims3-nx, GPL-3.0, v0.1.1). Raw NT device paths (Device/Harddisk0/Partition1 and
  // Device/Cdrom0, as they read here) name the same files as game: and d: do. A module loaded by bare name is joined
  // to the executable's folder, the first of them: without this its path never matched the registered one (The Sims
  // 3's sims3.xex), and it loaded with no recompiled code.
  constexpr std::array<std::string_view, 2> kDevicePrefixes = {"device/harddisk0/partition1/", "device/cdrom0/"};
  for (std::string_view prefix : kDevicePrefixes) {
    if (std::string_view(result).substr(0, prefix.size()) == prefix) {
      result.erase(0, prefix.size());
      break;
    }
  }

  return result;
}

}  // namespace rex::system
