/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/base/platform.h"

// Debug deliberately retains the missing-sequence assertion. This regression
// exercises the production NDEBUG failure result, not an assertion override.
#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64 && defined(NDEBUG)

#include <cstring>
#include <memory>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_assembler.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/memory.h"

DECLARE_bool(enable_incorrect_roundingmode_behavior);

namespace xe::cpu::testing {
namespace {

using namespace backend::x64;
using namespace hir;
using ppc::PPCContext;
// This fresh backend has no guest trampolines. Reuse an already committed HV
// page, without requiring a new native indirection-page allocation.
constexpr uint32_t kOldAddress = GUEST_TRAMPOLINE_BASE + 0x1000;
constexpr uint32_t kFailedAddress = kOldAddress + 4;
constexpr uint32_t kReusedAddress = kOldAddress + 8;

// Value-use allocation requires the owning builder to be current. These
// builders have overlapping lifetimes and unwind in reverse declaration order.
class FixtureHIR final : public HIRBuilder {
 public:
  FixtureHIR() : previous_(GetCurrent()) { MakeCurrent(); }
  ~FixtureHIR() override {
    if (GetCurrent() == this) {
      RemoveCurrent();
      if (previous_) {
        previous_->MakeCurrent();
      }
    }
  }

 private:
  HIRBuilder* const previous_;
};

// Read-only inspection of existing protected state. No selector override,
// injected failure, test hook or checkpoint/profile bypass is involved.
class InspectedEmitter final : public X64Emitter {
 public:
  using X64Emitter::X64Emitter;

  void CheckFailureCleanup() {
    CHECK(getSize() == 0);
    CHECK(epilog_label_ == nullptr);
    CHECK(tail_code_.empty());
    CHECK(label_cache_.empty());
    CHECK(guest_module_ == nullptr);
    CHECK(current_guest_function_ == 0);
    CHECK(current_instr_ == nullptr);
    CHECK(debug_info_ == nullptr);
    CHECK(debug_info_flags_ == 0);
    CHECK(trace_data_ == nullptr);
    CHECK(stack_size_ == 0);
    CHECK_FALSE(synchronize_stack_on_next_instruction_);
    CHECK(mxcsr_mode_ == MXCSRMode::Unknown);
    std::vector<SourceMapEntry> retained;
    source_map_arena_.CloneContents(&retained);
    CHECK(retained.empty());
  }
};

void Prefix(HIRBuilder& builder, uint32_t address) {
  builder.SourceOffset(address);
  builder.StoreContext(offsetof(PPCContext, r) + 3 * 8,
                       builder.LoadConstantUint64(0x123456789ABCDEF0ULL));
  // A supported F64 add creates actual cached labels and deferred MXCSR tail
  // code. Neither this prefix nor any other emitted guest code is invoked.
  auto a = builder.LoadContext(offsetof(PPCContext, f) + 4 * 8, FLOAT64_TYPE);
  auto b = builder.LoadContext(offsetof(PPCContext, f) + 5 * 8, FLOAT64_TYPE);
  builder.StoreContext(offsetof(PPCContext, f) + 3 * 8, builder.Add(a, b));
}

void MissingSequence(HIRBuilder& builder, uint32_t address) {
  Prefix(builder, address);
  auto a = builder.LoadContext(offsetof(PPCContext, r) + 4 * 8, INT32_TYPE);
  auto b = builder.LoadContext(offsetof(PPCContext, r) + 5 * 8, INT32_TYPE);
  // MAX accepts equal scalar integer types, but x64 registers only F32/F64/V128
  // MAX sequences. Nonconstant operands keep this in-range ordinary opcode
  // intact; no invalid opcode, type, register assignment or memory is forged.
  auto value = builder.Max(a, b);
  REQUIRE(value->def->GetOpcodeNum() == OPCODE_MAX);
  REQUIRE(value->type == INT32_TYPE);
  builder.StoreContext(offsetof(PPCContext, r) + 6 * 8, value);
  builder.Return();
  REQUIRE_FALSE(ppc::ContainsCheckpointHIR(builder));
}

void Compile(Processor& processor, HIRBuilder& builder) {
  REQUIRE(builder.Finalize());
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      processor.backend()->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
}

void CheckSourceMap(const std::vector<SourceMapEntry>& actual,
                    const std::vector<SourceMapEntry>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t n = 0; n < actual.size(); ++n) {
    CHECK(actual[n].guest_address == expected[n].guest_address);
    CHECK(actual[n].hir_offset == expected[n].hir_offset);
    CHECK(actual[n].code_offset == expected[n].code_offset);
  }
}

TEST_CASE("x64 missing sequence discards partial emission before publication",
          "[cpu][x64-emission-failure]") {
  // Do not change global settings to manufacture the deferred-work prefix.
  REQUIRE_FALSE(cvars::enable_incorrect_roundingmode_behavior);
  Memory memory;
  REQUIRE(memory.Initialize());
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
  REQUIRE(processor.compilation_options().profile ==
          CompilationProfile::kOrdinary);
  auto* native = static_cast<X64Backend*>(processor.backend());
  auto* cache = static_cast<X64CodeCache*>(native->code_cache());
  REQUIRE(cache->has_indirection_table());
  // Windows x64's committed indirection slots are at their guest addresses.
  auto* slots = reinterpret_cast<const uint32_t*>(uintptr_t{kOldAddress});
  MEMORY_BASIC_INFORMATION slot_region{};
  REQUIRE(VirtualQuery(slots, &slot_region, sizeof(slot_region)) ==
          sizeof(slot_region));
  REQUIRE(slot_region.State == MEM_COMMIT);
  REQUIRE(slot_region.Protect == PAGE_READWRITE);
  REQUIRE(slot_region.RegionSize >=
          reinterpret_cast<uintptr_t>(slots) -
              reinterpret_cast<uintptr_t>(slot_region.BaseAddress) + 0x1000);
  RawModule module(&processor);
  module.set_name("X64EmissionFailureTest");
  module.SetAddressRange(kOldAddress, 0x1000);
  const uint32_t failed_slot_before = slots[1];

  // Establish a real old function/placement with exactly the supported prefix
  // used before failure. This also confirms that prefix emits ordinary bytes.
  X64Assembler assembler(native);
  REQUIRE(assembler.Initialize());
  X64Function old_function(&module, kOldAddress);
  FixtureHIR old_builder;
  Prefix(old_builder, kOldAddress);
  old_builder.Return();
  Compile(processor, old_builder);
  REQUIRE(assembler.Assemble(&old_function, &old_builder, 0,
                             std::make_unique<FunctionDebugInfo>()));
  REQUIRE(old_function.machine_code());
  REQUIRE(old_function.machine_code_length() > 0);
  auto* old_code = old_function.machine_code();
  const size_t old_size = old_function.machine_code_length();
  const auto old_map = old_function.source_map();
  REQUIRE_FALSE(old_map.empty());
  const auto* old_debug = old_function.debug_info();
  const std::vector<uint8_t> old_bytes(old_code, old_code + old_size);
  const uint32_t old_slot = slots[0];
  REQUIRE(old_slot == reinterpret_cast<uintptr_t>(old_code));

  XbyakAllocator allocator;
  InspectedEmitter emitter(native, &allocator);
  X64Function failed_function(&module, kFailedAddress);
  FixtureHIR failed_builder;
  MissingSequence(failed_builder, kFailedAddress);
  Compile(processor, failed_builder);
  void* output = old_code;  // Stale usable output must be cleared on failure.
  size_t output_size = old_size;
  std::vector<SourceMapEntry> output_map;
  FunctionDebugInfo failed_debug;
  REQUIRE_FALSE(emitter.Emit(&failed_function, &failed_builder, 0,
                             &failed_debug, &output, &output_size,
                             &output_map));
  CHECK(output == nullptr);
  CHECK(output_size == 0);
  CHECK(output_map.empty());
  CHECK(failed_function.machine_code() == nullptr);
  CHECK(failed_function.machine_code_length() == 0);
  CHECK(failed_function.compiled_profile() == CompilationProfile::kUncompiled);
  CHECK(failed_function.source_map().empty());
  CHECK(slots[1] == failed_slot_before);
  emitter.CheckFailureCleanup();

  // Reuse the same public emitter after discarded unresolved labels/tail work.
  // Successful placement is allowed now, but the resulting code is never run.
  FixtureHIR reused_builder;
  reused_builder.SourceOffset(kFailedAddress);
  reused_builder.Nop();
  reused_builder.Return();
  Compile(processor, reused_builder);
  REQUIRE(emitter.Emit(&failed_function, &reused_builder, 0, nullptr, &output,
                       &output_size, &output_map));
  REQUIRE(output);
  CHECK(output_size > 0);
  REQUIRE(output_map.size() == 1);
  CHECK(output_map[0].guest_address == kFailedAddress);
  CHECK(slots[1] == reinterpret_cast<uintptr_t>(output));
  CHECK(cache->LookupFunction(reinterpret_cast<uint64_t>(output)) ==
        &failed_function);

  // Exercise the actual assembler's failure propagation with an existing valid
  // function: Setup/debug/source-map/indirection must retain their old values.
  FixtureHIR assembler_failed_builder;
  MissingSequence(assembler_failed_builder, kOldAddress);
  Compile(processor, assembler_failed_builder);
  REQUIRE_FALSE(assembler.Assemble(&old_function, &assembler_failed_builder, 0,
                                   std::make_unique<FunctionDebugInfo>()));
  CHECK(old_function.machine_code() == old_code);
  CHECK(old_function.machine_code_length() == old_size);
  CHECK(old_function.debug_info() == old_debug);
  CheckSourceMap(old_function.source_map(), old_map);
  CHECK(slots[0] == old_slot);
  CHECK(std::memcmp(old_code, old_bytes.data(), old_bytes.size()) == 0);
  CHECK(cache->LookupFunction(reinterpret_cast<uint64_t>(old_code)) ==
        &old_function);

  X64Function assembler_reused_function(&module, kReusedAddress);
  FixtureHIR assembler_reused_builder;
  assembler_reused_builder.SourceOffset(kReusedAddress);
  assembler_reused_builder.Nop();
  assembler_reused_builder.Return();
  Compile(processor, assembler_reused_builder);
  REQUIRE(assembler.Assemble(&assembler_reused_function,
                             &assembler_reused_builder, 0, nullptr));
  REQUIRE(assembler_reused_function.machine_code());
  CHECK(assembler_reused_function.machine_code_length() > 0);
  REQUIRE(assembler_reused_function.source_map().size() == 1);
  CHECK(assembler_reused_function.source_map()[0].guest_address ==
        kReusedAddress);
  CHECK(slots[2] ==
        reinterpret_cast<uintptr_t>(assembler_reused_function.machine_code()));
  CHECK(slots[0] == old_slot);
  CHECK(std::memcmp(old_code, old_bytes.data(), old_bytes.size()) == 0);
}

}  // namespace
}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64 && defined(NDEBUG)
