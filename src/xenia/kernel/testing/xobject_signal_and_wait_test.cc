/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject.h"

#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xmutant.h"

namespace xe {
namespace kernel {
namespace test {

TEST_CASE("Mutant signal and wait rejects a non-waitable target before release",
          "[kernel][mutant][signal-and-wait]") {
  auto signal = make_object<XMutant>(nullptr);
  signal->Initialize(false);
  // A real release would change the priority increment before checking its
  // owner. The guard must return before release or guest-thread TLS access.
  signal->set_priority_increment(7);
  auto wait = make_object<XObject>(XObject::Type::Undefined);
  uint64_t timeout = 0;

  CHECK(XObject::SignalAndWait(signal.get(), wait.get(), 3, 1, 0, &timeout) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(signal->priority_increment() == 7);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
