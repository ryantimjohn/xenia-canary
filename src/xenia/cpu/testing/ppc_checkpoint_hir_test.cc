/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"

#include <array>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/machine_info.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_assembler.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_assembler.h"
#endif

namespace xe::cpu::ppc::test {
namespace {

using namespace hir;
using Result = CheckpointHIRResult;
constexpr auto kUnproven = Result::kStructuralPairRequiresFullProducerProof;

class FixtureHIR : public HIRBuilder {
 public:
  FixtureHIR() { MakeCurrent(); }
  ~FixtureHIR() { RemoveCurrent(); }
};

uint32_t DForm(uint32_t opcode, uint32_t rt_rs, uint32_t ra, int16_t disp) {
  return (opcode << 26) | (rt_rs << 21) | (ra << 16) |
         static_cast<uint16_t>(disp);
}

Instr* Find(HIRBuilder& b, Opcode opcode) {
  for (auto block = b.first_block(); block; block = block->next) {
    for (auto i = block->instr_head; i; i = i->next) {
      if (i->opcode->num == opcode) {
        return i;
      }
    }
  }
  return nullptr;
}

// Evaluate the small integer HIR input graph, not a guest function or native
// code. This tests the constructed operands against concrete old register bits.
uint64_t EvaluateInteger(const Value* value,
                         const std::array<uint64_t, 32>& gprs) {
  REQUIRE(value);
  if (value->IsConstant()) {
    return value->type == INT32_TYPE ? value->constant.i32
                                     : value->constant.i64;
  }
  REQUIRE(value->def);
  const auto i = value->def;
  switch (i->opcode->num) {
    case OPCODE_LOAD_CONTEXT: {
      REQUIRE(i->src1.offset >= offsetof(PPCContext, r));
      const uint64_t offset = i->src1.offset - offsetof(PPCContext, r);
      REQUIRE(!(offset & 7));
      REQUIRE(offset / 8 < gprs.size());
      return gprs[offset / 8];
    }
    case OPCODE_ADD:
      return EvaluateInteger(i->src1.value, gprs) +
             EvaluateInteger(i->src2.value, gprs);
    case OPCODE_TRUNCATE:
      REQUIRE(value->type == INT32_TYPE);
      return static_cast<uint32_t>(EvaluateInteger(i->src1.value, gprs));
    default:
      FAIL("Unexpected integer input graph");
      return 0;
  }
}

backend::MachineInfo FixtureMachineInfo() {
  backend::MachineInfo info{};
  info.register_sets[0].id = 0;
  info.register_sets[0].types = backend::MachineInfo::RegisterSet::INT_TYPES;
  info.register_sets[0].count = 16;
  info.register_sets[1].id = 1;
  info.register_sets[1].types = backend::MachineInfo::RegisterSet::FLOAT_TYPES |
                                backend::MachineInfo::RegisterSet::VEC_TYPES;
  info.register_sets[1].count = 16;
  return info;
}

TEST_CASE("Checkpoint HIR retains an unfulfilled numeric pair",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  const uint32_t word = DForm(32, 3, 4, 16);
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, word));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  auto access = request->next;
  REQUIRE(access);
  CHECK(request->src1.offset == 1);
  CHECK(request->src2.offset == ((uint64_t{0x80001000} << 32) | word));
  CHECK(access->opcode == &OPCODE_PPC_CHECKPOINT_LWZ_info);
  CHECK(access->src2.offset == 1);
  CHECK(access->opcode->flags & OPCODE_FLAG_PAIRED_PREV);
  CHECK(request->opcode->flags & OPCODE_FLAG_VOLATILE);
  CHECK(VerifyCheckpointHIR(b) == kUnproven);
  CHECK(ContainsCheckpointHIR(b));
}

TEST_CASE("Checkpoint HIR RA0 is zero without a write to real r0",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 2, 0x80001004, DForm(36, 0, 0, -4)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  CHECK(request->src3.value->IsConstantZero());
  auto access = request->next;
  CHECK(access->src2.value->type == INT32_TYPE);
  CHECK(access->src2.value->constant.i32 == 0xFFFFFFFC);
  auto old_r0 = access->src3.value->def->src1.value->def;
  CHECK(old_r0->opcode == &OPCODE_LOAD_CONTEXT_info);
  CHECK(old_r0->src1.offset == offsetof(PPCContext, r));
  CHECK_FALSE(Find(b, OPCODE_STORE_CONTEXT));
  CHECK(VerifyCheckpointHIR(b) == kUnproven);
}

TEST_CASE("Checkpoint HIR keeps old RT and RA for aliased load",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 3, 0x80001008, DForm(32, 7, 7, -16)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  CHECK(request->src3.value->def->src1.offset ==
        offsetof(PPCContext, r) + 7 * 8);
  auto access = request->next;
  REQUIRE(access->next);
  CHECK(access->next->opcode == &OPCODE_ZERO_EXTEND_info);
  REQUIRE(access->next->next);
  CHECK(access->next->next->opcode == &OPCODE_STORE_CONTEXT_info);
  CHECK(access->next->next->src1.offset == offsetof(PPCContext, r) + 7 * 8);
  CHECK(VerifyCheckpointHIR(b) == kUnproven);
}

TEST_CASE("Checkpoint HIR narrows after signed address addition",
          "[cpu][checkpoint-hir]") {
  for (int16_t displacement : {int16_t{-32768}, int16_t{0}, int16_t{32767}}) {
    FixtureHIR b;
    REQUIRE(BuildCheckpointScalarPair(b, 4, 0x8000100C,
                                      DForm(36, 4, 4, displacement)));
    auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
    REQUIRE(request);
    auto ea = request->next->src2.value;
    REQUIRE(ea->def);
    CHECK(ea->type == INT32_TYPE);
    CHECK(ea->def->opcode == &OPCODE_TRUNCATE_info);
    auto sum = ea->def->src1.value;
    if (displacement) {
      REQUIRE(sum->def);
      CHECK(sum->def->opcode == &OPCODE_ADD_info);
      CHECK(sum->type == INT64_TYPE);
      CHECK(sum->def->src1.value == request->src3.value);
      CHECK(sum->def->src2.value->constant.i64 ==
            static_cast<uint64_t>(static_cast<int64_t>(displacement)));
    } else {
      CHECK(sum == request->src3.value);
    }
    CHECK(VerifyCheckpointHIR(b) == kUnproven);
  }
}

TEST_CASE("Unsupported checkpoint construction appends nothing",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  CHECK_FALSE(BuildCheckpointScalarPair(b, 0, 0x80001000, DForm(32, 3, 4, 0)));
  CHECK_FALSE(BuildCheckpointScalarPair(b, 1, 0x80001001, DForm(32, 3, 4, 0)));
  for (uint32_t opcode : {31u, 33u, 37u, 46u, 47u}) {
    CHECK_FALSE(
        BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(opcode, 3, 4, 0)));
  }
  CHECK_FALSE(b.first_block());
  CHECK(VerifyCheckpointHIR(b) == Result::kNoPairs);
}

TEST_CASE("Checkpoint constructed EA wraps and store uses old aliased source",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 4, 4, 32)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  std::array<uint64_t, 32> old{};
  old[4] = 0xCAFEBABEFFFFFFF0ULL;
  CHECK(EvaluateInteger(request->next->src2.value, old) == 0x10);
  CHECK(EvaluateInteger(request->next->src3.value, old) == 0xFFFFFFF0);
  CHECK(EvaluateInteger(request->src3.value, old) == old[4]);
  CHECK_FALSE(Find(b, OPCODE_STORE_CONTEXT));
  CHECK(VerifyCheckpointHIR(b) == kUnproven);
}

TEST_CASE("Checkpoint pair identity is unique across the HIR function",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 0)));
  auto tail = b.last_instr();
  CHECK_FALSE(BuildCheckpointScalarPair(b, 1, 0x80001004, DForm(32, 3, 4, 0)));
  CHECK(b.last_instr() == tail);
  REQUIRE(BuildCheckpointScalarPair(b, 2, 0x80001004, DForm(36, 3, 4, 0)));
  auto second = b.last_instr()->prev;
  second->src1.offset = 1;
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidIdentity);
}

TEST_CASE("Checkpoint verifier rejects identity and word conflicts",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 16)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  auto access = request->next;
  access->src1.offset = 2;
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidIdentity);
  access->src1.offset = 1;
  request->src2.offset = (uint64_t{0x80001000} << 32) | DForm(37, 3, 4, 16);
  CHECK(VerifyCheckpointHIR(b) == Result::kUnsupportedInstruction);
}

TEST_CASE("Checkpoint verifier rejects every interposed effect",
          "[cpu][checkpoint-hir]") {
  for (int kind = 0; kind < 5; ++kind) {
    FixtureHIR b;
    REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 0)));
    auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
    REQUIRE(request);
    auto access = request->next;
    switch (kind) {
      case 0:
        b.StoreContext(offsetof(PPCContext, r), b.LoadConstantInt64(1));
        break;
      case 1:
        b.Store(b.LoadConstantInt64(0x1000), b.LoadConstantInt32(1));
        break;
      case 2:
        b.ContextBarrier();
        break;
      case 3:
        b.CallIndirect(b.LoadConstantInt64(0x80002000));
        break;
      case 4:
        b.Branch(b.NewLabel());
        break;
    }
    b.last_instr()->MoveBefore(access);
    CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOrdering);
  }
}

TEST_CASE("Checkpoint verifier rejects cross-block and orphaned accesses",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 0)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  auto access = request->next;
  auto original_block = access->block;
  b.MarkLabel(b.NewLabel());
  access->block = b.current_block();
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOrdering);
  access->block = original_block;
  request->UnlinkAndNOP();
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOrdering);
  CHECK(ContainsCheckpointHIR(b));
}

TEST_CASE("Checkpoint verifier rejects changed EA and load writeback",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(32, 3, 4, 0)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  auto access = request->next;
  auto original_ea = access->src1.value;
  access->set_src1(b.LoadConstantInt64(0x1000));
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOperands);
  access->set_src1(original_ea);
  access->next->next->src1.offset = offsetof(PPCContext, r) + 4 * 8;
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOperands);
}

TEST_CASE("Checkpoint verifier rejects stale old base after context mutation",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 16)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  b.StoreContext(offsetof(PPCContext, r) + 4 * 8, b.LoadConstantInt64(0));
  b.last_instr()->MoveBefore(request);
  CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOperands);
}

TEST_CASE("Checkpoint verifier rejects constant-marked dynamic producer values",
          "[cpu][checkpoint-hir]") {
  for (int target = 0; target < 3; ++target) {
    FixtureHIR b;
    REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(32, 3, 4, 16)));
    auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
    REQUIRE(request);
    auto access = request->next;
    REQUIRE(access);
    CHECK(VerifyCheckpointHIR(b) == kUnproven);
    switch (target) {
      case 0:
        // set_constant retains the old producing definition pointer.
        access->src1.value->def->src1.value->set_constant(uint64_t{0x1010});
        break;
      case 1:
        access->dest->set_constant(uint32_t{0x12345678});
        break;
      case 2:
        access->next->dest->set_constant(uint64_t{0x12345678});
        break;
    }
    CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOperands);
  }
}

TEST_CASE(
    "Checkpoint verifier requires matching dynamic definition destinations",
    "[cpu][checkpoint-hir]") {
  for (int target = 0; target < 6; ++target) {
    FixtureHIR b;
    const bool load = target != 3;
    REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000,
                                      DForm(load ? 32 : 36, 3, 4, 16)));
    auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
    REQUIRE(request);
    auto access = request->next;
    REQUIRE(access);
    CHECK(VerifyCheckpointHIR(b) == kUnproven);
    Value* value = nullptr;
    switch (target) {
      case 0:
        value = request->src3.value;  // Old GPR load.
        break;
      case 1:
        value = access->src1.value;  // EA truncate.
        break;
      case 2:
        value = access->src1.value->def->src1.value;  // EA add.
        break;
      case 3:
        value = access->src3.value;  // Old store source truncate.
        break;
      case 4:
        value = access->dest;  // Load result.
        break;
      case 5:
        value = access->next->dest;  // Zero-extended load result.
        break;
    }
    REQUIRE(value);
    REQUIRE(value->def);
    if (target < 4) {
      value->def->dest = b.LoadConstantInt64(0);
    } else {
      // Redirect the value's definition while leaving the access/writeback
      // destinations and their use relationships unchanged.
      value->def = request->src3.value->def;
    }
    CHECK(VerifyCheckpointHIR(b) == Result::kInvalidOperands);
  }
}

TEST_CASE("Dormant checkpoint recipe runs actual structural passes",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  auto first = b.LoadContext(offsetof(PPCContext, r) + 4 * 8, INT64_TYPE);
  b.StoreContext(offsetof(PPCContext, r) + 5 * 8, first);
  auto second = b.LoadContext(offsetof(PPCContext, r) + 4 * 8, INT64_TYPE);
  b.StoreContext(offsetof(PPCContext, r) + 6 * 8, second);
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(32, 3, 4, 16)));
  b.Return();
  REQUIRE(b.Finalize());
  auto info = FixtureMachineInfo();
  compiler::Compiler compiler(nullptr);
  REQUIRE(AddDormantCheckpointPasses(compiler, &info));
  CHECK(compiler.Compile(&b));
  CHECK(second->def->opcode == &OPCODE_LOAD_CONTEXT_info);
  CHECK(second->reg.set != nullptr);
  CHECK(VerifyCheckpointHIR(b) == kUnproven);
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  CHECK(request->next->opcode == &OPCODE_PPC_CHECKPOINT_LWZ_info);
  CHECK(request->block->ordinal == 0);
}

TEST_CASE("Dormant recipe invokes the real pair verifier",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 0)));
  auto request = Find(b, OPCODE_PPC_CHECKPOINT_REQUEST);
  REQUIRE(request);
  auto access = request->next;
  b.ContextBarrier();
  b.last_instr()->MoveBefore(access);
  b.Return();
  REQUIRE(b.Finalize());
  auto info = FixtureMachineInfo();
  compiler::Compiler compiler(nullptr);
  CHECK_FALSE(AddDormantCheckpointPasses(compiler, nullptr));
  REQUIRE(AddDormantCheckpointPasses(compiler, &info));
  CHECK_FALSE(compiler.Compile(&b));
}

TEST_CASE("Dormant requests fail native assembly before emitter initialization",
          "[cpu][checkpoint-hir]") {
  FixtureHIR b;
  REQUIRE(BuildCheckpointScalarPair(b, 1, 0x80001000, DForm(36, 3, 4, 0)));
#if XE_ARCH_AMD64
  backend::x64::X64Assembler assembler(nullptr);
  CHECK_FALSE(assembler.Assemble(nullptr, &b, 0, nullptr));
#elif XE_ARCH_ARM64
  backend::a64::A64Assembler assembler(nullptr);
  CHECK_FALSE(assembler.Assemble(nullptr, &b, 0, nullptr));
#endif
}

}  // namespace
}  // namespace xe::cpu::ppc::test
