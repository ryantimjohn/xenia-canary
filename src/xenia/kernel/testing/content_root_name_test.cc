/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_root_name.h"

#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("A root name a title really uses is accepted", "[content][root]") {
  // The two 4D5308ED uses, and the shapes a save package takes.
  CHECK(IsSafeContentRootName("stexemplar"));
  CHECK(IsSafeContentRootName("Save_01"));
  CHECK(IsSafeContentRootName("a"));
  CHECK(IsSafeContentRootName("ID-0123456789"));
  CHECK(IsSafeContentRootName(std::string(64, 'x')));
}

TEST_CASE("A root name that could leave the cache root is refused",
          "[content][root]") {
  // Each of these becomes a host path segment in xeXamContentOpenFile, so
  // each of them would name a file outside cache_root/content_files.
  CHECK_FALSE(IsSafeContentRootName(".."));
  CHECK_FALSE(IsSafeContentRootName("a/b"));
  CHECK_FALSE(IsSafeContentRootName("a\\b"));
  CHECK_FALSE(IsSafeContentRootName("c:"));
  CHECK_FALSE(IsSafeContentRootName("../../windows/system32/drivers/etc"));
  CHECK_FALSE(IsSafeContentRootName("."));
}

TEST_CASE("A root name that is empty or too long is refused",
          "[content][root]") {
  CHECK_FALSE(IsSafeContentRootName(""));
  CHECK_FALSE(IsSafeContentRootName(std::string(65, 'x')));
}

TEST_CASE("Anything but an identifier character is refused",
          "[content][root]") {
  // A guest string is bytes, not text: a space, a wildcard, a stream name, a
  // reserved device name's colon and a non-ASCII byte are all refused, and so
  // is an embedded NUL, which would truncate the name a host API sees.
  CHECK_FALSE(IsSafeContentRootName("save 01"));
  CHECK_FALSE(IsSafeContentRootName("save*"));
  CHECK_FALSE(IsSafeContentRootName("save.dat"));
  CHECK_FALSE(IsSafeContentRootName(std::string("save\0me", 7)));
  CHECK_FALSE(
      IsSafeContentRootName("sav\x80"
                            "e"));
  CHECK_FALSE(
      IsSafeContentRootName("sav\xFF"
                            "e"));
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
