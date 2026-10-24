/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"

#include <cstdint>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace test {

TEST_CASE("64-bit shim results preserve every bit in r3", "[kernel][shim]") {
  const uint64_t values[] = {
      0x0000000000000000ull, 0x0000000000000001ull, 0x0000000080000000ull,
      0x0000000100000000ull, 0x123456789ABCDEF0ull, 0x8000000000000000ull,
      0xDEADF00DDEADF00Dull, 0xFFFFFFFFFFFFFFFFull,
  };
  PPCContext context = {};
  for (uint64_t value : values) {
    CAPTURE(value);
    qword_result_t result(value);
    result.Store(&context);
    CHECK(context.r[3] == value);
  }
}

TEST_CASE("Unsigned 32-bit shim results retain sign extension in r3",
          "[kernel][shim]") {
  const struct {
    uint32_t value;
    uint64_t expected;
  } cases[] = {
      {0x00000000u, 0x0000000000000000ull},
      {0x7FFFFFFFu, 0x000000007FFFFFFFull},
      {0x80000000u, 0xFFFFFFFF80000000ull},
      {0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull},
  };
  PPCContext context = {};
  for (const auto& test_case : cases) {
    CAPTURE(test_case.value);
    dword_result_t result(test_case.value);
    result.Store(&context);
    CHECK(context.r[3] == test_case.expected);
  }
}

TEST_CASE("Signed 32-bit shim results retain sign extension in r3",
          "[kernel][shim]") {
  PPCContext context = {};
  int_result_t negative_result(-1);
  negative_result.Store(&context);
  CHECK(context.r[3] == 0xFFFFFFFFFFFFFFFFull);

  int_result_t minimum_result(std::numeric_limits<int32_t>::min());
  minimum_result.Store(&context);
  CHECK(context.r[3] == 0xFFFFFFFF80000000ull);

  int_result_t positive_result(std::numeric_limits<int32_t>::max());
  positive_result.Store(&context);
  CHECK(context.r[3] == 0x000000007FFFFFFFull);
}

TEST_CASE("32-bit shim error codes retain sign extension in r3",
          "[kernel][shim]") {
  PPCContext context = {};
  X_HRESULT_result_t hresult(X_E_FAIL);
  hresult.Store(&context);
  CHECK(context.r[3] == 0xFFFFFFFF80004005ull);

  dword_result_t status(X_STATUS_UNSUCCESSFUL);
  status.Store(&context);
  CHECK(context.r[3] == 0xFFFFFFFFC0000001ull);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
