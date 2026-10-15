/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <array>
#include <cstring>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/test_module.h"
#include "xenia/cpu/testing/util.h"

DECLARE_bool(zero_page_scalar_first_chance);
DECLARE_bool(protect_zero);
DECLARE_bool(full_optimization_even_with_debug);

namespace xe::cpu::testing {
namespace {

constexpr uint32_t kCode = 0x81000000;
constexpr uint32_t kLiR10One = (14u << 26) | (10u << 21) | 1;
constexpr uint32_t kLiR10Two = (14u << 26) | (10u << 21) | 2;
constexpr uint32_t kBlr = 0x4E800020;

uint32_t ScalarWord(bool write, uint32_t ra = 0) {
  return ((write ? 36u : 32u) << 26) | ((write ? 10u : 11u) << 21) | (ra << 16);
}

struct Options {
  bool enabled = cvars::zero_page_scalar_first_chance;
  bool protect = cvars::protect_zero;
  bool optimize = cvars::full_optimization_even_with_debug;
  Options() {
    cvars::zero_page_scalar_first_chance = true;
    cvars::protect_zero = true;
    cvars::full_optimization_even_with_debug = true;
  }
  ~Options() {
    cvars::zero_page_scalar_first_chance = enabled;
    cvars::protect_zero = protect;
    cvars::full_optimization_even_with_debug = optimize;
  }
};

class ScalarModule final : public Module {
 public:
  explicit ScalarModule(Processor* processor) : Module(processor) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  bool ContainsAddress(uint32_t address) override {
    return address >= kCode && address < kCode + 16;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(
        processor_->backend()->CreateGuestFunction(this, address));
  }

 private:
  const std::string name_ = "synthetic-scalar";
};

struct Fixture {
  std::unique_ptr<Memory> memory = std::make_unique<Memory>();
  std::unique_ptr<Processor> processor;
  std::unique_ptr<ThreadState> thread;
  ThreadState* previous = ThreadState::Get();
  uint32_t stack = 0;

  explicit Fixture(uint32_t word) {
    REQUIRE(memory->Initialize());
    auto* heap = memory->LookupHeap(kCode);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(kCode, 0x10000, 0x10000,
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    const std::array<uint32_t, 4> code = {kLiR10One, word, kLiR10Two, kBlr};
    for (size_t index = 0; index < code.size(); ++index) {
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(kCode + static_cast<uint32_t>(index) * 4),
          code[index]);
    }
    processor = std::make_unique<Processor>(memory.get(), nullptr);
    REQUIRE(processor->Setup(std::make_unique<backend::x64::X64Backend>()));
    REQUIRE(
        processor->AddModule(std::make_unique<ScalarModule>(processor.get())));
    processor->backend()->CommitExecutableRange(kCode, kCode + 0x10000);
    stack = memory->SystemHeapAlloc(0x10000);
    REQUIRE(stack);
    thread =
        std::make_unique<ThreadState>(processor.get(), 0x100, stack + 0x10000);
    ThreadState::Bind(thread.get());
    thread->context()->lr = 0xBCBCBCBC;
    // RA=0 must not use the nonzero value in guest r0.
    thread->context()->r[0] = 0x123456789ABCDEF0;
    thread->context()->r[10] = 0xCCCC;
    thread->context()->r[11] = 0xDDDD;
  }

  ~Fixture() {
    ThreadState::Bind(previous);
    thread.reset();
    if (stack) {
      memory->SystemHeapFree(stack);
    }
  }

  GuestFunction* Resolve() {
    auto* function = processor->ResolveFunction(kCode);
    REQUIRE(function);
    REQUIRE(function->is_guest());
    return static_cast<GuestFunction*>(function);
  }
};

struct ExpectedFault {
  uint64_t code_begin;
  uint64_t code_end;
  uint64_t expected_address;
  ULONG_PTR expected_operation;
  uint64_t observed_pc = 0;
};

int CaptureExpectedFault(EXCEPTION_POINTERS* exception,
                         ExpectedFault* expected) {
  const auto* record = exception->ExceptionRecord;
  const auto pc = reinterpret_cast<uint64_t>(record->ExceptionAddress);
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record->NumberParameters < 2 || pc < expected->code_begin ||
      pc >= expected->code_end ||
      record->ExceptionInformation[0] != expected->expected_operation ||
      record->ExceptionInformation[1] != expected->expected_address) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  expected->observed_pc = pc;
  return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ locals requiring destruction in this SEH leaf. The expected native
// access fault unwinds the call; it never resumes or skips the instruction.
bool InvokeWithExpectedFault(Function* function, ThreadState* thread,
                             ExpectedFault* expected) {
  __try {
    function->Call(thread, 0xBCBCBCBC);
    return false;
  } __except (CaptureExpectedFault(GetExceptionInformation(), expected)) {
    return true;
  }
}

TEST_CASE(
    "Scalar first chance observes materialized state before real zero fault",
    "[cpu][scalar-first-chance]") {
  const bool write = GENERATE(false, true);
  INFO("Native access is write: " << write);
  Options options;
  Fixture fixture(ScalarWord(write));
  auto* function = fixture.Resolve();
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(write));
  REQUIRE(site);
  size_t callbacks = 0;
  ppc::PPCContext saved{};
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState* thread,
          const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(thread == fixture.thread.get());
        CHECK(fault.guest_pc == kCode + 4);
        CHECK(fault.raw_instruction == ScalarWord(write));
        CHECK(fault.guest_address == 0);
        CHECK(fault.is_write == write);
        CHECK(fault.modeled_context->r[10] == 1);
        CHECK(fault.modeled_context->r[0] == 0x123456789ABCDEF0);
        std::memcpy(&saved, fault.modeled_context, sizeof(saved));
        // Nested offers must decline. Replacing the registry inside the
        // callback also proves that no registry lock spans guest invocation.
        site->Call(thread, 0xBCBCBCBC);
        CHECK(callbacks == 1);
        {
          ThreadState nested(fixture.processor.get(), 0x101);
          site->Call(&nested, 0xBCBCBCBC);
          CHECK(callbacks == 1);
          CHECK(ThreadState::Get() == thread);
        }
        fixture.processor->set_scalar_access_first_chance_handler({});
        thread->context()->r[10] = 0xBAD;
        thread->context()->r[0] = 0;
        thread->context()->ctr = 0xBAD;
        thread->context()->f[7] = -1.0;
      });
  ExpectedFault fault{
      reinterpret_cast<uint64_t>(function->machine_code()),
      reinterpret_cast<uint64_t>(function->machine_code()) +
          function->machine_code_length(),
      reinterpret_cast<uint64_t>(fixture.memory->TranslateVirtual(0)),
      write ? ULONG_PTR(1) : ULONG_PTR(0)};
  REQUIRE(InvokeWithExpectedFault(function, fixture.thread.get(), &fault));
  CHECK(callbacks == 1);
  CHECK(function->MapMachineCodeToGuestAddress(fault.observed_pc) == kCode + 4);
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
  CHECK(fixture.thread->context()->r[10] == 1);  // Tail overwrite did not run.
  CHECK(fixture.thread->context()->r[11] == 0xDDDD);
}

TEST_CASE(
    "Scalar first chance leaves ordinary nonzero native accesses unchanged",
    "[cpu][scalar-first-chance]") {
  const bool write = GENERATE(false, true);
  Options options;
  Fixture fixture(ScalarWord(write, 4));
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance&) {
        ++callbacks;
      });
  const uint32_t data = fixture.memory->SystemHeapAlloc(4, 4);
  REQUIRE(data);
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(data),
                               0x12345678);
  fixture.thread->context()->r[4] = 0xDEAD000000000000ULL | data;
  REQUIRE(fixture.Resolve()->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
  CHECK(fixture.thread->context()->r[10] == 2);
  if (write) {
    CHECK(xe::load_and_swap<uint32_t>(fixture.memory->TranslateVirtual(data)) ==
          1);
  } else {
    CHECK(fixture.thread->context()->r[11] == 0x12345678);
  }
  fixture.memory->SystemHeapFree(data);
}

TEST_CASE("Scalar first chance rejects stale instruction and module identities",
          "[cpu][scalar-first-chance]") {
  Options options;
  Fixture fixture(ScalarWord(true));
  auto* function = fixture.Resolve();
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true));
  REQUIRE(site);
  cvars::zero_page_scalar_first_chance = false;
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true)));
  cvars::zero_page_scalar_first_chance = true;
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 5, ScalarWord(true)));
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, 37u << 26));  // Updating STWU is unsupported.
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true, 4)));  // Dynamic RA unsupported.
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true) | 0xFFFF));  // Negative literal.
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance&) {
        ++callbacks;
      });
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(kCode + 4),
                               kBlr);
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(kCode + 4),
                               ScalarWord(true));
  fixture.processor->RemoveModule("synthetic-scalar");
  REQUIRE(fixture.processor->AddModule(
      std::make_unique<ScalarModule>(fixture.processor.get())));
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
}

void ChangeOperand(ppc::PPCContext* context, void* count, void*) {
  ++*static_cast<size_t*>(count);
  CHECK(context->r[10] == 1);
  context->r[10] = 7;
}

TEST_CASE(
    "Scalar first chance restores context and nesting after host exception",
    "[cpu][scalar-first-chance]") {
  Options options;
  Fixture fixture(ScalarWord(true));
  auto* function = fixture.Resolve();
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true));
  REQUIRE(site);
  fixture.thread->context()->r[10] = 1;
  ppc::PPCContext saved;
  std::memcpy(&saved, fixture.thread->context(), sizeof(saved));
  struct CallbackFailure {};
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState* thread,
          const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(fault.modeled_context->r[10] == 1);
        thread->context()->r[10] = 0xBAD;
        thread->context()->f[7] = -1.0;
        throw CallbackFailure{};
      });
  // Direct builtin invocation tests host-exception cleanup, without making a
  // claim that the guest instruction ran or requiring unwinding through JIT.
  REQUIRE_THROWS_AS(site->Call(fixture.thread.get(), 0xBCBCBCBC),
                    CallbackFailure);
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(fault.modeled_context->r[10] == 1);
      });
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 2);  // The first callback's TLS nesting guard was reset.
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
}

TEST_CASE("Scalar boundary control reloads native operands after CALL_EXTERN",
          "[cpu][scalar-first-chance]") {
  Options options;
  size_t calls = 0;
  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  REQUIRE(processor->Setup(std::make_unique<backend::x64::X64Backend>()));
  auto* callback = processor->DefineBuiltin("ChangeScalarOperand",
                                            ChangeOperand, &calls, nullptr);
  auto module = std::make_unique<TestModule>(
      processor.get(), "scalar-boundary-control",
      [](uint32_t address) { return address == kCode; },
      [callback](hir::HIRBuilder& builder) {
        StoreGPR(builder, 10, builder.LoadConstantUint64(1));
        // Give context promotion a cached value before the call.
        StoreGPR(builder, 12, LoadGPR(builder, 10));
        builder.CallExtern(callback);
        builder.Store(LoadGPR(builder, 4),
                      builder.Truncate(LoadGPR(builder, 10), hir::INT32_TYPE));
        StoreGPR(builder, 10, builder.LoadConstantUint64(2));
        builder.Return();
        return true;
      });
  REQUIRE(processor->AddModule(std::move(module)));
  processor->backend()->CommitExecutableRange(kCode, kCode + 0x10000);
  auto* function = processor->ResolveFunction(kCode);
  REQUIRE(function);
  const uint32_t allocation = memory->SystemHeapAlloc(0x10000);
  REQUIRE(allocation);
  auto thread = std::make_unique<ThreadState>(processor.get(), 0x100,
                                              allocation + 0x10000);
  thread->context()->lr = 0xBCBCBCBC;
  thread->context()->r[4] = allocation;
  REQUIRE(function->Call(thread.get(), 0xBCBCBCBC));
  CHECK(calls == 1);
  uint32_t stored = 0;
  std::memcpy(&stored, memory->TranslateVirtual(allocation), sizeof(stored));
  CHECK(stored == 7);  // Real native STORE consumes the post-call operand.
  CHECK(thread->context()->r[10] == 2);
  CHECK(thread->context()->r[12] == 1);
  thread.reset();
  memory->SystemHeapFree(allocation);
}

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
