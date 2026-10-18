// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/cpu/ppc/guest_gpr_checkpoint.h"

#include <array>
#include <cstring>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/testing/util.h"

DECLARE_uint32(guest_gpr_capture_pc);
DECLARE_uint32(guest_gpr_observe_pc);
DECLARE_uint32(guest_gpr_capture_key);
DECLARE_uint32(guest_gpr_observe_key);
DECLARE_uint32(guest_gpr_capture_mask);
DECLARE_uint32(guest_gpr_observe_mask);
DECLARE_int32(guest_gpr_predicate_gpr);
DECLARE_uint32(guest_gpr_predicate_value);
DECLARE_uint32(guest_gpr_delay_ms);
DECLARE_bool(debug);
DECLARE_bool(full_optimization_even_with_debug);
DECLARE_bool(store_all_context_values);
DECLARE_bool(disable_context_promotion);

namespace xe::cpu::testing {
namespace {
using ppc::GuestGPRCheckpointCollector;
using ppc::GuestGPRCheckpointConfig;

GuestGPRCheckpointConfig Config() {
  return {0x1000, 0x2000, 31, 30, uint32_t{1} << 4, uint32_t{1} << 11,
          11,     0,      10};
}

TEST_CASE("Guest GPR checkpoint validates configuration",
          "[guest-gpr-checkpoint]") {
  CHECK(GuestGPRCheckpointConfig{}.disabled());
  CHECK(Config().Validate() == nullptr);
  auto config = Config();
  config.observe_pc = config.capture_pc;
  CHECK(config.Validate() != nullptr);
  config = Config();
  config.capture_pc |= 1;
  CHECK(config.Validate() != nullptr);
  config = Config();
  config.capture_key_gpr = 32;
  CHECK(config.Validate() != nullptr);
  config = Config();
  config.predicate_gpr = -2;
  CHECK(config.Validate() != nullptr);
  config = Config();
  config.capture_mask = 0x7F;
  CHECK(config.Validate() != nullptr);
  config = Config();
  config.observe_pc = 0;
  CHECK(config.Validate() != nullptr);
}

TEST_CASE("Guest GPR cache joins full keys and threads without spending rows",
          "[guest-gpr-checkpoint]") {
  GuestGPRCheckpointCollector collector(Config());
  std::array<uint64_t, 32> regs{};
  regs[31] = regs[30] = 0x100000005;
  regs[4] = 123;
  CHECK_FALSE(collector.Visit(true, 1, regs.data(), 100));
  CHECK_FALSE(collector.Visit(false, 1, regs.data(), 109));
  regs[11] = 1;
  CHECK_FALSE(collector.Visit(false, 1, regs.data(), 110));
  regs[11] = 0x100000000;  // Predicate is low32, output retains full64.
  auto row = collector.Visit(false, 1, regs.data(), 112);
  REQUIRE(row);
  CHECK(row->sequence == 1);
  CHECK(row->matched);
  CHECK(row->capture_age_ms == 12);
  CHECK(row->captured[0] == 123);
  CHECK(row->observed[0] == 0x100000000);
  CHECK_FALSE(collector.Visit(false, 1, regs.data(), 113));
  CHECK_FALSE(collector.Visit(true, 1, regs.data(), 114));
  row = collector.Visit(false, 1, regs.data(), 115);
  REQUIRE(row);
  CHECK(row->capture_sequence == 2);
  CHECK(row->sequence == 2);
  row = collector.Visit(false, 2, regs.data(), 116);
  REQUIRE(row);
  CHECK_FALSE(row->matched);  // No cross-thread join.
  regs[30] = 5;
  row = collector.Visit(false, 1, regs.data(), 117);
  REQUIRE(row);
  CHECK_FALSE(row->matched);  // No low32 key collision.
}

TEST_CASE("Guest GPR collector has bounded slots and saturating rows",
          "[guest-gpr-checkpoint]") {
  auto config = Config();
  config.delay_ms = 0;
  config.predicate_gpr = -1;
  GuestGPRCheckpointCollector collector(config);
  std::array<uint64_t, 32> regs{};
  for (uint64_t key = 0; key < 16; ++key) {
    regs[31] = key;
    CHECK_FALSE(collector.Visit(true, 1, regs.data(), 100));
  }
  regs[31] = 16;
  CHECK_FALSE(collector.Visit(true, 1, regs.data(), 100));  // Evicts key zero.
  regs[30] = 16;
  auto latest = collector.Visit(false, 1, regs.data(), 101);
  REQUIRE(latest);
  CHECK(latest->matched);  // Later readers are retained after startup fills.
  CHECK(latest->cache_evictions == 1);
  regs[30] = 0;
  auto row = collector.Visit(false, 1, regs.data(), 101);
  REQUIRE(row);
  CHECK(row->cache_evictions == 2);
  CHECK_FALSE(row->matched);
  CHECK_FALSE(collector.Visit(false, 1, regs.data(), 102));
  regs[30] = 0;
  for (uint64_t n = 1; n < 63; ++n) {
    regs[11] = n;
    row = collector.Visit(false, 1, regs.data(), 103 + n);
    REQUIRE(row);
    CHECK(row->cap_reached == (n == 62));
  }
  regs[11] = 999;
  CHECK_FALSE(collector.Visit(false, 1, regs.data(), 999));
  CHECK(collector.last_observation()->sequence == 64);
}

TEST_CASE("Guest GPR observation budget is shared by concurrent callers",
          "[guest-gpr-checkpoint]") {
  auto config = Config();
  config.delay_ms = 0;
  config.predicate_gpr = -1;
  GuestGPRCheckpointCollector collector(config);
  std::vector<std::thread> workers;
  for (uint32_t thread = 0; thread < 4; ++thread) {
    workers.emplace_back([&, thread] {
      std::array<uint64_t, 32> regs{};
      for (uint64_t n = 0; n < 64; ++n) {
        regs[11] = n;
        collector.Visit(false, thread, regs.data(), 100 + n);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  REQUIRE(collector.last_observation());
  CHECK(collector.last_observation()->sequence == 64);
}

struct CheckpointCvars {
  GuestGPRCheckpointConfig old{
      cvars::guest_gpr_capture_pc,    cvars::guest_gpr_observe_pc,
      cvars::guest_gpr_capture_key,   cvars::guest_gpr_observe_key,
      cvars::guest_gpr_capture_mask,  cvars::guest_gpr_observe_mask,
      cvars::guest_gpr_predicate_gpr, cvars::guest_gpr_predicate_value,
      cvars::guest_gpr_delay_ms};
  bool debug = cvars::debug;
  bool full = cvars::full_optimization_even_with_debug;
  bool stores = cvars::store_all_context_values;
  bool promotion = cvars::disable_context_promotion;
  static void Set(const GuestGPRCheckpointConfig& c) {
    cvars::guest_gpr_capture_pc = c.capture_pc;
    cvars::guest_gpr_observe_pc = c.observe_pc;
    cvars::guest_gpr_capture_key = c.capture_key_gpr;
    cvars::guest_gpr_observe_key = c.observe_key_gpr;
    cvars::guest_gpr_capture_mask = c.capture_mask;
    cvars::guest_gpr_observe_mask = c.observe_mask;
    cvars::guest_gpr_predicate_gpr = c.predicate_gpr;
    cvars::guest_gpr_predicate_value = c.predicate_value;
    cvars::guest_gpr_delay_ms = c.delay_ms;
  }
  ~CheckpointCvars() {
    Set(old);
    cvars::debug = debug;
    cvars::full_optimization_even_with_debug = full;
    cvars::store_all_context_values = stores;
    cvars::disable_context_promotion = promotion;
  }
};

TEST_CASE("Malformed checkpoints disable without failing processor startup",
          "[guest-gpr-checkpoint]") {
  CheckpointCvars restore;
  auto invalid = Config();
  invalid.capture_key_gpr = 32;
  CheckpointCvars::Set(invalid);
  TestFunction test([](hir::HIRBuilder& builder) { builder.Return(); });
  REQUIRE_FALSE(test.processors.empty());
  for (const auto& processor : test.processors) {
    CHECK(processor->frontend()->guest_gpr_checkpoints() == nullptr);
  }
}

TEST_CASE("Compiled PPC checkpoint sees preinstruction values at branch target",
          "[guest-gpr-checkpoint][backend]") {
  CheckpointCvars restore;
  cvars::debug = false;
  cvars::full_optimization_even_with_debug = true;
  cvars::store_all_context_values = false;
  cvars::disable_context_promotion = false;
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t address = 0x82000000;
  REQUIRE(memory.LookupHeap(address)->AllocFixed(
      address, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  // addi r3,0,5; b capture; addi r3,0,99 (skipped);
  // capture: addi r3,r3,1; observe: addi r3,r3,2; blr.
  const uint32_t code[] = {0x38600005, 0x48000008, 0x38600063,
                           0x38630001, 0x38630002, 0x4E800020};
  for (size_t n = 0; n < std::size(code); ++n) {
    store_and_swap<uint32_t>(memory.TranslateVirtual(address + uint32_t(n * 4)),
                             code[n]);
  }
  auto run = [&](bool enabled) {
    CheckpointCvars::Set({enabled ? address + 12 : 0,
                          enabled ? address + 16 : 0, 4, 4, 1u << 3, 1u << 3,
                          -1, 0, 0});
    Processor processor(&memory, nullptr);
#if XE_ARCH_AMD64
    REQUIRE(processor.Setup(std::make_unique<backend::x64::X64Backend>()));
#elif XE_ARCH_ARM64
    REQUIRE(processor.Setup(std::make_unique<backend::a64::A64Backend>()));
#endif
    auto module = std::make_unique<RawModule>(&processor);
    module->set_executable(true);
    module->SetAddressRange(address, sizeof(code));
    REQUIRE(processor.AddModule(std::move(module)));
    auto* function = processor.ResolveFunction(address);
    REQUIRE(function);
    const uint32_t stack = memory.SystemHeapAlloc(0x10000);
    REQUIRE(stack);
    // Destruction before SystemHeapFree keeps the actual guest stack owned.
    PPCContext result{};
    {
      ThreadState thread(&processor, 1, stack + 0x10000);
      auto* context = thread.context();
      for (size_t n = 0; n < 32; ++n) {
        context->r[n] = 100 + n;
        context->f[n] = double(n) + 0.5;
      }
      context->r[1] = stack + 0xFF00;
      context->lr = 0xBCBCBCBC;
      context->ctr = 123;
      context->set_cr(0x12345678);
      for (size_t n = 0; n < 128; ++n)
        context->v[n] = vec128i(uint32_t(n), 2, 3, 4);
      processor.backend()->SetGuestRoundingMode(context, 0);
      REQUIRE(function->Call(&thread, uint32_t(context->lr)));
      result = *context;
      if (enabled) {
        auto* collector = processor.frontend()->guest_gpr_checkpoints();
        REQUIRE(collector);
        const auto row = collector->last_observation();
        REQUIRE(row);
        CHECK(row->matched);
        CHECK(row->captured[0] == 5);
        CHECK(row->observed[0] == 6);
        CHECK(row->sequence == 1);
        // Configuration is snapshotted, not reread at each execution.
        CheckpointCvars::Set({});
        REQUIRE(function->Call(&thread, uint32_t(context->lr)));
        CHECK(collector->last_observation()->capture_sequence == 2);
      } else {
        CHECK(processor.frontend()->guest_gpr_checkpoints() == nullptr);
      }
    }
    memory.SystemHeapFree(stack);
    result.r[1] = 0;  // Allocator-selected stack identity differs between runs.
    return result;
  };
  const auto without = run(false);
  const auto with = run(true);
  CHECK(with.r[3] == 8);
  CHECK(std::memcmp(with.r, without.r, sizeof(with.r)) == 0);
  CHECK(std::memcmp(with.f, without.f, sizeof(with.f)) == 0);
  CHECK(std::memcmp(with.v, without.v, sizeof(with.v)) == 0);
  CHECK(with.cr() == without.cr());
  CHECK(with.lr == without.lr);
  CHECK(with.ctr == without.ctr);
  CHECK(with.msr == without.msr);
  CHECK(with.fpscr.value == without.fpscr.value);
  CHECK(with.xer_ca == without.xer_ca);
  CHECK(with.xer_ov == without.xer_ov);
  CHECK(with.xer_so == without.xer_so);
  CHECK(with.vrsave == without.vrsave);
  CHECK(with.vscr_sat == without.vscr_sat);
  CHECK(std::memcmp(&with.vscr_vec, &without.vscr_vec, sizeof(with.vscr_vec)) ==
        0);
}
}  // namespace
}  // namespace xe::cpu::testing
