/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_CONTENT_ROOT_NAME_H_
#define XENIA_KERNEL_XAM_CONTENT_ROOT_NAME_H_

#include <cstddef>
#include <string_view>

namespace xe {
namespace kernel {
namespace xam {

// The longest root name that is staged. Titles use short ones ("stexemplar"
// is ten characters); this is only here so that a guest string that is long
// rather than malformed cannot reach a host path length limit.
constexpr size_t kMaximumContentRootNameLength = 64;

// True when a root name may be used as a directory name under the cache root.
//
// XamContentOpenFile stages the package it is given at
// cache_root/content_files/<title id>/<root name>/<entry name>, and the root
// name is a string the guest chose. A name holding "..", a separator or a
// drive letter would leave that directory and CopyGuestFileToHost would
// truncate and overwrite whatever it named, so what is accepted here is a
// plain identifier and nothing else: no separators, no dots, no colons, no
// spaces, no NUL, and nothing outside ASCII. Header-only and kernel-free so
// that the kernel test suite can call it.
inline bool IsSafeContentRootName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumContentRootNameLength) {
    return false;
  }
  for (const char c : name) {
    const bool is_identifier_character =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!is_identifier_character) {
      return false;
    }
  }
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_CONTENT_ROOT_NAME_H_
