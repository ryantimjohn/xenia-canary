/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_tracers.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DECLARE_bool(emit_source_annotations);
DECLARE_bool(instrument_call_times);

namespace xe::cpu::ppc::test {
namespace {

using namespace backend::x64;
using namespace hir;
constexpr uint32_t kStorePC = 0x80001000;
constexpr uint32_t kAfterStorePC = kStorePC + 4;
constexpr size_t kDestination = offsetof(PPCContext, r) + 3 * sizeof(uint64_t);
constexpr uint64_t kValue = 0x12345678;
constexpr size_t kMaxDraftBytes = 4096;
constexpr size_t kMaxStoreBytes = 16;

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

struct OpaqueDraft {
  std::vector<uint8_t> bytes;
  std::vector<SourceMapEntry> source_map;
};

// The protected seam emits scratch bytes without the public function setup or
// cleanup. AutoGrow label relocations remain unresolved: no ready(), placement,
// registration, function pointer, or invocation is provided by this fixture.
class DraftEmitter final : public X64Emitter {
 public:
  using X64Emitter::X64Emitter;
  ~DraftEmitter() override { Discard(); }

  bool EmitOpaque(HIRBuilder& builder, OpaqueDraft& draft) {
    draft = {};
    if (processor()->compilation_options().profile !=
            CompilationProfile::kOrdinary ||
        ContainsCheckpointHIR(builder) ||
        HIRBuilder::GetCurrent() != &builder) {
      return false;
    }
    REQUIRE(getSize() == 0);
    REQUIRE(epilog_label_ == nullptr);
    REQUIRE(tail_code_.empty());
    REQUIRE(label_cache_.empty());
    REQUIRE(guest_module_ == nullptr);
    REQUIRE(current_guest_function_ == 0);
    REQUIRE(debug_info_ == nullptr);
    REQUIRE(debug_info_flags_ == 0);
    REQUIRE(trace_data_ == nullptr);
    EmitFunctionInfo info{};
    if (!X64Emitter::Emit(&builder, info) || !getSize() ||
        getSize() > kMaxDraftBytes) {
      Discard();
      return false;
    }
    // Copy opaque storage only; no scratch address escapes this method.
    const uint8_t* scratch = getCode();
    draft.bytes.assign(scratch, scratch + getSize());
    source_map_arena_.CloneContents(&draft.source_map);
    Discard();
    return true;
  }

  void CheckDiscarded() {
    CHECK(getSize() == 0);
    CHECK(epilog_label_ == nullptr);
    CHECK(tail_code_.empty());
    CHECK(label_cache_.empty());
    CHECK(current_instr_ == nullptr);
    CHECK(stack_size_ == 0);
    CHECK_FALSE(synchronize_stack_on_next_instruction_);
    CHECK(mxcsr_mode_ == MXCSRMode::Unknown);
    std::vector<SourceMapEntry> retained;
    source_map_arena_.CloneContents(&retained);
    CHECK(retained.empty());
  }

 private:
  void Discard() {
    // Clear Xbyak's references before destroying labels or captured callbacks.
    reset();
    tail_code_.clear();
    for (auto* label : label_cache_) {
      delete label;
    }
    label_cache_.clear();
    epilog_label_ = nullptr;
    source_map_arena_.Reset();
    guest_module_ = nullptr;
    current_guest_function_ = 0;
    current_instr_ = nullptr;
    debug_info_ = nullptr;
    debug_info_flags_ = 0;
    trace_data_ = nullptr;
    stack_size_ = 0;
    synchronize_stack_on_next_instruction_ = false;
    ForgetMxcsrMode();
  }
};

struct Decoder {
  csh handle = 0;
  cs_insn* instruction = nullptr;
  Decoder() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK &&
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK) {
      instruction = cs_malloc(handle);
    }
  }
  ~Decoder() {
    if (instruction) {
      cs_free(instruction, 1);
    }
    if (handle) {
      cs_close(&handle);
    }
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

void CheckConstantStore(const OpaqueDraft& draft) {
  REQUIRE(draft.source_map.size() == 2);
  REQUIRE(draft.source_map[0].guest_address == kStorePC);
  REQUIRE(draft.source_map[1].guest_address == kAfterStorePC);
  const size_t begin = draft.source_map[0].code_offset;
  const size_t end = draft.source_map[1].code_offset;
  REQUIRE(begin < end);
  REQUIRE(end <= draft.bytes.size());
  REQUIRE(end - begin <= kMaxStoreBytes);
  Decoder decoder;
  REQUIRE(decoder.instruction);
  const uint8_t* cursor = draft.bytes.data() + begin;
  size_t remaining = end - begin;
  uint64_t address = begin;  // Numeric draft offset, never a native address.
  REQUIRE(cs_disasm_iter(decoder.handle, &cursor, &remaining, &address,
                         decoder.instruction));
  // Exactly one MOV consumes the interval. This rejects extra memory effects,
  // branches, calls, and helpers rather than treating undecoded bytes as safe.
  REQUIRE(remaining == 0);
  REQUIRE(decoder.instruction->id == X86_INS_MOV);
  REQUIRE(decoder.instruction->detail);
  const auto& x = decoder.instruction->detail->x86;
  REQUIRE(x.op_count == 2);
  const auto& destination = x.operands[0];
  REQUIRE(destination.type == X86_OP_MEM);
  CHECK(destination.size == sizeof(uint64_t));
  CHECK(destination.access == CS_AC_WRITE);
  // RSI is the actual backend's GetContextReg contract, not a guest ABI.
  CHECK(destination.mem.base == X86_REG_RSI);
  CHECK(destination.mem.index == X86_REG_INVALID);
  CHECK(destination.mem.segment == X86_REG_INVALID);
  CHECK(destination.mem.disp == kDestination);
  REQUIRE(x.operands[1].type == X86_OP_IMM);
  CHECK(static_cast<uint64_t>(x.operands[1].imm) == kValue);
  cs_regs reads{}, writes{};
  uint8_t read_count = 0, write_count = 0;
  REQUIRE(cs_regs_access(decoder.handle, decoder.instruction, reads,
                         &read_count, writes, &write_count) == CS_ERR_OK);
  REQUIRE(read_count == 1);
  CHECK(reads[0] == X86_REG_RSI);
  CHECK(write_count == 0);
}

TEST_CASE("Actual x64 draft contains one canonical constant context store",
          "[cpu][x64-draft-store]") {
  // Prerequisites are observed, never changed to manufacture the interval.
  REQUIRE(GetTracingMode() == 0);
  REQUIRE_FALSE(cvars::trace_functions);
  REQUIRE_FALSE(cvars::trace_function_coverage);
  REQUIRE_FALSE(cvars::trace_function_references);
  REQUIRE_FALSE(cvars::trace_function_data);
  REQUIRE_FALSE(cvars::emit_source_annotations);
  REQUIRE_FALSE(cvars::instrument_call_times);
  Memory memory;
  REQUIRE(memory.Initialize());
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::make_unique<X64Backend>()));
  REQUIRE(processor.compilation_options().profile ==
          CompilationProfile::kOrdinary);
  FixtureHIR builder;
  builder.SourceOffset(kStorePC);
  builder.StoreContext(kDestination, builder.LoadConstantUint64(kValue));
  builder.SourceOffset(kAfterStorePC);
  builder.Return();
  REQUIRE(builder.Finalize());
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  namespace passes = compiler::passes;
  compiler::Compiler compiler(&processor);
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::RegisterAllocationPass>(
      processor.backend()->machine_info()));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  REQUIRE(compiler.Compile(&builder));
  REQUIRE_FALSE(ContainsCheckpointHIR(builder));
  XbyakAllocator allocator;
  DraftEmitter emitter(static_cast<X64Backend*>(processor.backend()),
                       &allocator);
  OpaqueDraft draft;
  REQUIRE(emitter.EmitOpaque(builder, draft));
  emitter.CheckDiscarded();
  CheckConstantStore(draft);
  // Only the source-delimited, relocation-free store is inspected. Prolog,
  // epilog, helper tails and unresolved branches carry no execution claim;
  // neither complete bank capture nor checkpoint production is established.
}

}  // namespace
}  // namespace xe::cpu::ppc::test

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
