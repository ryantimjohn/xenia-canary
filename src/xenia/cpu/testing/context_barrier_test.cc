/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstddef>
#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/compiler/passes/context_promotion_pass.h"
#include "xenia/cpu/compiler/passes/dead_code_elimination_pass.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DECLARE_bool(full_optimization_even_with_debug);

namespace xe::cpu::testing {
namespace {

class CurrentHIR final : public hir::HIRBuilder {
 public:
  CurrentHIR() { MakeCurrent(); }
};

// Exercise store elimination regardless of the test runner's debug settings.
class EnableContextStoreElimination {
 public:
  EnableContextStoreElimination()
      : previous_(cvars::full_optimization_even_with_debug) {
    cvars::full_optimization_even_with_debug = true;
  }
  ~EnableContextStoreElimination() {
    cvars::full_optimization_even_with_debug = previous_;
  }

 private:
  bool previous_;
};

constexpr size_t GPROffset(size_t index) {
  return offsetof(ppc::PPCContext, r) + sizeof(uint64_t) * index;
}

size_t CountOpcode(const hir::HIRBuilder& builder, hir::Opcode opcode) {
  size_t count = 0;
  for (auto block = builder.first_block(); block; block = block->next) {
    for (auto instr = block->instr_head; instr; instr = instr->next) {
      count += instr->opcode->num == opcode;
    }
  }
  return count;
}

TEST_CASE("Context barriers retain stores before later context overwrites",
          "[cpu][context-barrier]") {
  const bool barrier = GENERATE(false, true);
  INFO("Context barrier present: " << barrier);
  EnableContextStoreElimination options;
  CurrentHIR builder;
  builder.StoreContext(GPROffset(3),
                       builder.LoadConstantUint64(0x123456789ABC));
  auto before = builder.last_instr();
  if (barrier) {
    builder.ContextBarrier();
  }
  builder.StoreContext(GPROffset(3),
                       builder.LoadConstantUint64(0xFEDCBA987654));
  builder.Return();
  REQUIRE(builder.Finalize());

  compiler::Compiler compiler(nullptr);
  compiler::passes::ContextPromotionPass promotion;
  REQUIRE(promotion.Initialize(&compiler));
  REQUIRE(promotion.Run(&builder));
  // Without a barrier the earlier store is dead. With one it must be visible
  // at the boundary, even though the same context field is overwritten later.
  CHECK(before->opcode->num ==
        (barrier ? hir::OPCODE_STORE_CONTEXT : hir::OPCODE_NOP));

  compiler::passes::DeadCodeEliminationPass dce;
  REQUIRE(dce.Initialize(&compiler));
  REQUIRE(dce.Run(&builder));
  CHECK(CountOpcode(builder, hir::OPCODE_STORE_CONTEXT) == (barrier ? 2 : 1));
  CHECK(CountOpcode(builder, hir::OPCODE_CONTEXT_BARRIER) == (barrier ? 1 : 0));
}

TEST_CASE("Context barriers invalidate promoted context loads",
          "[cpu][context-barrier]") {
  const bool barrier = GENERATE(false, true);
  INFO("Context barrier present: " << barrier);
  EnableContextStoreElimination options;
  CurrentHIR builder;
  auto before = builder.LoadContext(GPROffset(3), hir::INT64_TYPE);
  builder.StoreContext(GPROffset(4), before);
  if (barrier) {
    builder.ContextBarrier();
  }
  auto after = builder.LoadContext(GPROffset(3), hir::INT64_TYPE);
  builder.StoreContext(GPROffset(5), after);
  builder.Return();
  REQUIRE(builder.Finalize());

  compiler::Compiler compiler(nullptr);
  compiler::passes::ContextPromotionPass promotion;
  REQUIRE(promotion.Initialize(&compiler));
  REQUIRE(promotion.Run(&builder));
  CHECK(after->def->opcode->num ==
        (barrier ? hir::OPCODE_LOAD_CONTEXT : hir::OPCODE_ASSIGN));

  compiler::passes::DeadCodeEliminationPass dce;
  REQUIRE(dce.Initialize(&compiler));
  REQUIRE(dce.Run(&builder));
  CHECK(CountOpcode(builder, hir::OPCODE_LOAD_CONTEXT) == (barrier ? 2 : 1));
  CHECK(CountOpcode(builder, hir::OPCODE_STORE_CONTEXT) == 2);
  CHECK(CountOpcode(builder, hir::OPCODE_CONTEXT_BARRIER) == (barrier ? 1 : 0));
}

}  // namespace
}  // namespace xe::cpu::testing
