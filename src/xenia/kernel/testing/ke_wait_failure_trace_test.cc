// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; SDK-derived ancestry must never be upstreamed.
#include "xenia/kernel/util/native_object_failure.h"

#include <array>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xobject.h"

namespace xe::kernel::util::test {

TEST_CASE("Ke wait failure log admission never exceeds sixteen under contention",
          "[ke_wait_failure_trace]") {
  WaitFailureLogBudget budget;
  std::array<std::atomic<uint32_t>, 17> seen{};
  std::atomic<uint32_t> ready{0}, accepted{0}, invalid{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 8; ++i) {
    workers.emplace_back([&] {
      ++ready;
      while (!go.load()) std::this_thread::yield();
      for (unsigned n = 0; n != 64; ++n) {
        const auto sequence = budget.TryClaim();
        if (!sequence) continue;
        ++accepted;
        if (sequence > 16) ++invalid;
        else ++seen[sequence];
      }
    });
  }
  while (ready.load() != 8) std::this_thread::yield();
  go = true;
  for (auto& worker : workers) worker.join();
  CHECK(accepted.load() == 16);
  CHECK(invalid.load() == 0);
  for (unsigned sequence = 1; sequence <= 16; ++sequence) {
    CHECK(seen[sequence].load() == 1);
  }
  CHECK_FALSE(budget.HasCapacity());
  for (unsigned n = 0; n != 64; ++n) CHECK(budget.TryClaim() == 0);
}

// The existing unsupported resolver branch calls assert_always(). Do not
// bypass that assertion or intentionally trigger it in assert-enabled builds.
// Release/NDEBUG has TWO cases in this filter; assert-enabled builds have ONE.
#if defined(NDEBUG)
TEST_CASE("Ke resolver diagnostics distinguish unsupported native first use",
          "[ke_wait_failure_trace]") {
  constexpr uint32_t signature = 0x11223344;
  constexpr uint32_t unrelated_blink = 0x55667788;
  for (auto type : {TimerNotificationObject, TimerSynchronizationObject,
                    ThreadObject, UndefinedObject}) {
    X_DISPATCH_HEADER header{};
    header.type = type;
    header.signal_state = 1;
    header.wait_list.flink_ptr = signature;
    header.wait_list.blink_ptr = unrelated_blink;
    const auto before = header;
    NativeObjectFailure failure;
    // The unsupported branch does not use KernelState or touch guest memory.
    auto object = XObject::GetNativeObject(nullptr, &header, UndefinedObject,
                                           false, &failure);
    CHECK_FALSE(bool(object));
    CHECK(failure.reason == NativeObjectFailure::Reason::kUnsupportedFirstUseType);
    CHECK(failure.effective_type == uint32_t(type));
    CHECK(failure.header_type_read);
    CHECK(failure.header_type == uint32_t(type));
    CHECK(failure.signature_word == signature);
    CHECK_FALSE(failure.stashed_handle_read);
    CHECK(failure.stashed_handle == 0);
    CHECK(std::memcmp(&before, &header, sizeof(header)) == 0);
    // The default-disabled resolver path remains valid without diagnostic output.
    CHECK_FALSE(bool(XObject::GetNativeObject(nullptr, &header)));
    CHECK(std::memcmp(&before, &header, sizeof(header)) == 0);
  }
  X_DISPATCH_HEADER header{};
  header.type = EventNotificationObject;
  header.wait_list.flink_ptr = signature;
  NativeObjectFailure failure;
  CHECK_FALSE(bool(XObject::GetNativeObject(nullptr, &header,
      TimerSynchronizationObject, false, &failure)));
  CHECK(failure.effective_type == uint32_t(TimerSynchronizationObject));
  CHECK_FALSE(failure.header_type_read);
  CHECK(failure.header_type == 0);
}
#endif

}  // namespace xe::kernel::util::test
