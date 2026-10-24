/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"

#include <unordered_set>

#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe::cpu::ppc {
namespace {

using namespace hir;

bool IsCheckpointOpcode(Opcode opcode) {
  return opcode == OPCODE_PPC_CHECKPOINT_REQUEST ||
         opcode == OPCODE_PPC_CHECKPOINT_LWZ ||
         opcode == OPCODE_PPC_CHECKPOINT_STW;
}

bool SupportedWord(uint32_t pc, uint32_t word) {
  return !(pc & 3) && ((word >> 26) == 32 || (word >> 26) == 36);
}

int32_t Displacement(uint32_t word) {
  const uint32_t raw = word & 0xFFFF;
  return raw < 0x8000 ? static_cast<int32_t>(raw)
                      : static_cast<int32_t>(raw) - 0x10000;
}

bool Constant64(const Value* value, uint64_t expected) {
  return value && value->type == INT64_TYPE && value->IsConstant() &&
         value->constant.i64 == expected;
}

// Deliberately conservative: local spill/reload provenance is not proved by
// this scaffold. The post-allocation verifier rejects a changed operand shape.
bool PureBeforeRequest(const Instr* definition, const Instr* request) {
  if (!definition || definition->block != request->block) {
    return false;
  }
  for (auto i = definition->next; i; i = i->next) {
    if (i == request) {
      return true;
    }
    if ((i->opcode->flags &
         (OPCODE_FLAG_BRANCH | OPCODE_FLAG_MEMORY | OPCODE_FLAG_VOLATILE)) ||
        i->opcode->num == OPCODE_STORE_CONTEXT ||
        i->opcode->num == OPCODE_CONTEXT_BARRIER ||
        i->opcode->num == OPCODE_STORE_LOCAL) {
      return false;
    }
  }
  return false;
}

bool OldGPR(const Value* value, uint32_t reg, const Instr* request) {
  return value && value->type == INT64_TYPE && !value->IsConstant() &&
         value->def && value->def->dest == value &&
         value->def->opcode->num == OPCODE_LOAD_CONTEXT &&
         value->def->src1.offset == offsetof(PPCContext, r) + reg * 8 &&
         PureBeforeRequest(value->def, request);
}

bool EffectiveAddress(const Value* ea, const Value* base, uint32_t word,
                      const Instr* request) {
  if (!ea || ea->type != INT32_TYPE) {
    return false;
  }
  const int32_t displacement = Displacement(word);
  if (!((word >> 16) & 31)) {
    return ea->IsConstant() &&
           ea->constant.i32 == static_cast<uint32_t>(displacement);
  }
  if (ea->IsConstant() || !ea->def || ea->def->dest != ea ||
      ea->def->opcode->num != OPCODE_TRUNCATE ||
      !PureBeforeRequest(ea->def, request)) {
    return false;
  }
  const Value* sum = ea->def->src1.value;
  if (!displacement) {
    return sum == base;
  }
  return sum && sum->type == INT64_TYPE && !sum->IsConstant() && sum->def &&
         sum->def->dest == sum && sum->def->opcode->num == OPCODE_ADD &&
         !sum->def->flags && sum->def->src1.value == base &&
         Constant64(
             sum->def->src2.value,
             static_cast<uint64_t>(static_cast<int64_t>(displacement))) &&
         PureBeforeRequest(sum->def, request);
}

bool StoreSource(const Value* source, uint32_t reg, const Instr* request) {
  return source && source->type == INT32_TYPE && !source->IsConstant() &&
         source->def && source->def->dest == source &&
         source->def->opcode->num == OPCODE_TRUNCATE &&
         PureBeforeRequest(source->def, request) &&
         OldGPR(source->def->src1.value, reg, request);
}

bool LoadWriteback(const Instr* access, uint32_t reg) {
  auto extend = access->next;
  auto store = extend ? extend->next : nullptr;
  return access->dest && access->dest->type == INT32_TYPE &&
         !access->dest->IsConstant() && access->dest->def == access && extend &&
         store && extend->block == access->block &&
         store->block == access->block &&
         extend->opcode->num == OPCODE_ZERO_EXTEND && extend->dest &&
         extend->dest->type == INT64_TYPE && !extend->dest->IsConstant() &&
         extend->dest->def == extend && extend->src1.value == access->dest &&
         store->opcode->num == OPCODE_STORE_CONTEXT &&
         store->src1.offset == offsetof(PPCContext, r) + reg * 8 &&
         store->src2.value == extend->dest;
}

bool CapturePrefix(const Block* block, uint32_t guest_pc, const Instr* stop) {
  const auto gpr_offset = [](uint64_t offset) {
    return offset >= offsetof(PPCContext, r) &&
           offset < offsetof(PPCContext, r) + sizeof(PPCContext::r) &&
           (offset - offsetof(PPCContext, r)) % sizeof(uint64_t) == 0;
  };
  std::unordered_set<const Value*> defined;
  const auto input = [&](const Value* value) {
    return value && value->type == INT64_TYPE &&
           (value->IsConstant() || defined.count(value));
  };
  uint64_t previous_pc = 0;
  bool has_marker = false;
  for (auto i = block->instr_head; i != stop; i = i->next) {
    if (!i || i->block != block || i->flags) {
      return false;
    }
    switch (i->opcode->num) {
      case OPCODE_SOURCE_OFFSET:
        if ((i->src1.offset & 3) || i->src1.offset > guest_pc ||
            (has_marker && i->src1.offset <= previous_pc)) {
          return false;
        }
        previous_pc = i->src1.offset;
        has_marker = true;
        continue;
      case OPCODE_LOAD_CONTEXT:
        if (!has_marker || !gpr_offset(i->src1.offset) || !i->dest ||
            i->dest->type != INT64_TYPE) {
          return false;
        }
        break;
      case OPCODE_STORE_CONTEXT:
        if (!has_marker || !gpr_offset(i->src1.offset) ||
            !input(i->src2.value) || i->dest) {
          return false;
        }
        continue;
      case OPCODE_ADD:
        if (!has_marker || !input(i->src1.value) || !input(i->src2.value) ||
            !i->dest || i->dest->type != INT64_TYPE) {
          return false;
        }
        break;
      default:
        return false;
    }
    if (i->dest->IsConstant() || i->dest->def != i ||
        !defined.insert(i->dest).second) {
      return false;
    }
  }
  return true;
}

bool BuildDormantCheckpointGprCaptureImpl(hir::HIRBuilder& builder,
                                          uint32_t guest_pc,
                                          uint32_t raw_instruction,
                                          DormantCheckpointGprCapture* output,
                                          bool after_call) {
  if (!output || output->ea_slot || HIRBuilder::GetCurrent() != &builder ||
      !SupportedWord(guest_pc, raw_instruction) || !builder.locals().empty()) {
    return false;
  }
  for (auto slot : output->gpr_slots) {
    if (slot) {
      return false;
    }
  }
  auto block = builder.current_block();
  if (!block || block != builder.last_block() || block->next ||
      block->label_head || block->label_tail || block->incoming_edge_head ||
      block->outgoing_edge_head || block->incoming_values ||
      !block->instr_tail ||
      block->instr_tail->opcode->num != OPCODE_SOURCE_OFFSET ||
      block->instr_tail->src1.offset != guest_pc) {
    return false;
  }
  if (!after_call) {
    if (block != builder.first_block() || block->prev) {
      return false;
    }
  } else {
    // Before Finalize/CFG, reject every alternative HIR route: exactly two
    // unlabelled blocks, no pre-existing analysis, and no transfer other than
    // the final ordinary direct call in the predecessor. This is a construction
    // snapshot, not a proof against later builder mutation or runtime
    // nonreturn.
    const auto predecessor = block->prev;
    if (!predecessor || predecessor != builder.first_block() ||
        predecessor->prev || predecessor->next != block ||
        predecessor->label_head || predecessor->label_tail ||
        predecessor->incoming_edge_head || predecessor->outgoing_edge_head ||
        predecessor->incoming_values) {
      return false;
    }
    const auto call = predecessor->instr_tail;
    const auto lr_store = call ? call->prev : nullptr;
    const auto return_address = lr_store ? lr_store->prev : nullptr;
    const auto marker = return_address ? return_address->prev : nullptr;
    if (!call || call->block != predecessor ||
        call->opcode->num != OPCODE_CALL || call->flags || call->dest ||
        !call->src1.symbol || !call->src1.symbol->is_guest() ||
        !call->src1.symbol->address() || (call->src1.symbol->address() & 3) ||
        call->src2.value || call->src3.value || !lr_store ||
        lr_store->opcode->num != OPCODE_STORE_CONTEXT || lr_store->flags ||
        lr_store->block != predecessor || lr_store->dest ||
        lr_store->src1.offset != offsetof(PPCContext, lr) || !return_address ||
        return_address->opcode->num != OPCODE_SET_RETURN_ADDRESS ||
        return_address->flags || return_address->block != predecessor ||
        return_address->dest || !marker || marker->block != predecessor ||
        marker->opcode->num != OPCODE_SOURCE_OFFSET || marker->flags ||
        marker->src1.offset > UINT32_MAX - 4 ||
        marker->src1.offset + 4 > guest_pc ||
        !Constant64(return_address->src1.value, marker->src1.offset + 4) ||
        !Constant64(lr_store->src2.value, marker->src1.offset + 4) ||
        !block->instr_head ||
        block->instr_head->opcode->num != OPCODE_SOURCE_OFFSET ||
        block->instr_head->src1.offset != marker->src1.offset + 4 ||
        !CapturePrefix(predecessor, static_cast<uint32_t>(marker->src1.offset),
                       return_address)) {
      return false;
    }
  }
  if (!CapturePrefix(block, guest_pc, nullptr)) {
    return false;
  }
  // All rejection checks precede HIR/local allocation and output mutation.
  std::array<Value*, 32> values{};
  DormantCheckpointGprCapture result;
  for (size_t reg = 0; reg < values.size(); ++reg) {
    values[reg] =
        builder.LoadContext(offsetof(PPCContext, r) + reg * 8, INT64_TYPE);
    result.gpr_slots[reg] = builder.AllocLocal(INT64_TYPE);
  }
  result.ea_slot = builder.AllocLocal(INT32_TYPE);
  for (size_t reg = 0; reg < values.size(); ++reg) {
    builder.StoreLocal(result.gpr_slots[reg], values[reg]);
  }
  const auto ra = (raw_instruction >> 16) & 31;
  auto base = ra ? values[ra] : builder.LoadConstantInt64(0);
  auto ea = builder.Truncate(
      builder.Add(base,
                  builder.LoadConstantInt64(Displacement(raw_instruction))),
      INT32_TYPE);
  builder.StoreLocal(result.ea_slot, ea);
  *output = result;
  return true;
}

}  // namespace

bool BuildDormantCheckpointGprCapture(hir::HIRBuilder& builder,
                                      uint32_t guest_pc,
                                      uint32_t raw_instruction,
                                      DormantCheckpointGprCapture* output) {
  return BuildDormantCheckpointGprCaptureImpl(builder, guest_pc,
                                              raw_instruction, output, false);
}

bool BuildDormantPostCallCheckpointGprCapture(
    hir::HIRBuilder& builder, uint32_t guest_pc, uint32_t raw_instruction,
    DormantCheckpointGprCapture* output) {
  return BuildDormantCheckpointGprCaptureImpl(builder, guest_pc,
                                              raw_instruction, output, true);
}

bool BuildCheckpointScalarPair(hir::HIRBuilder& builder, uint64_t pair_id,
                               uint32_t guest_pc, uint32_t raw_instruction) {
  if (HIRBuilder::GetCurrent() != &builder || !pair_id ||
      !SupportedWord(guest_pc, raw_instruction)) {
    return false;
  }
  for (auto block = builder.first_block(); block; block = block->next) {
    for (auto i = block->instr_head; i; i = i->next) {
      if (i->opcode->num == OPCODE_PPC_CHECKPOINT_REQUEST &&
          i->src1.offset == pair_id) {
        return false;
      }
    }
  }
  const uint32_t ra = (raw_instruction >> 16) & 31;
  const uint32_t rt_rs = (raw_instruction >> 21) & 31;
  const bool load = (raw_instruction >> 26) == 32;
  auto base =
      ra ? builder.LoadContext(offsetof(PPCContext, r) + ra * 8, INT64_TYPE)
         : builder.LoadConstantInt64(0);
  // All inputs are old values, including RA==RS; actual r0 is not overwritten
  // when RA=0 supplies a literal zero base. Narrow after signed addition.
  auto source = load ? nullptr
                     : builder.Truncate(
                           builder.LoadContext(
                               offsetof(PPCContext, r) + rt_rs * 8, INT64_TYPE),
                           INT32_TYPE);
  auto ea = builder.Truncate(
      builder.Add(base,
                  builder.LoadConstantInt64(Displacement(raw_instruction))),
      INT32_TYPE);
  builder.PPCCheckpointRequest(pair_id, guest_pc, raw_instruction, base);
  if (load) {
    auto value = builder.PPCCheckpointLwz(pair_id, ea);
    builder.StoreContext(offsetof(PPCContext, r) + rt_rs * 8,
                         builder.ZeroExtend(value, INT64_TYPE));
  } else {
    builder.PPCCheckpointStw(pair_id, ea, source);
  }
  return true;
}

CheckpointHIRResult VerifyCheckpointHIR(const hir::HIRBuilder& builder) {
  std::unordered_set<uint64_t> ids;
  for (auto block = builder.first_block(); block; block = block->next) {
    for (auto i = block->instr_head; i; i = i->next) {
      const auto opcode = i->opcode->num;
      if (!IsCheckpointOpcode(opcode)) {
        continue;
      }
      if (opcode != OPCODE_PPC_CHECKPOINT_REQUEST) {
        if (!i->prev || i->prev->opcode->num != OPCODE_PPC_CHECKPOINT_REQUEST ||
            i->prev->block != block) {
          return CheckpointHIRResult::kInvalidOrdering;
        }
        continue;  // The preceding request validates the complete pair.
      }
      const uint64_t id = i->src1.offset;
      if (!id || !ids.insert(id).second) {
        return CheckpointHIRResult::kInvalidIdentity;
      }
      const uint32_t pc = static_cast<uint32_t>(i->src2.offset >> 32);
      const uint32_t word = static_cast<uint32_t>(i->src2.offset);
      if (!SupportedWord(pc, word)) {
        return CheckpointHIRResult::kUnsupportedInstruction;
      }
      const bool load = (word >> 26) == 32;
      auto access = i->next;
      if (!access || access->block != block ||
          access->opcode->num !=
              (load ? OPCODE_PPC_CHECKPOINT_LWZ : OPCODE_PPC_CHECKPOINT_STW)) {
        return CheckpointHIRResult::kInvalidOrdering;
      }
      if ((load ? access->src2.offset : access->src1.offset) != id) {
        return CheckpointHIRResult::kInvalidIdentity;
      }
      const uint32_t ra = (word >> 16) & 31;
      const uint32_t rt_rs = (word >> 21) & 31;
      const Value* base = i->src3.value;
      if (i->dest || i->flags || access->flags ||
          (ra ? !OldGPR(base, ra, i) : !Constant64(base, 0)) ||
          !EffectiveAddress(load ? access->src1.value : access->src2.value,
                            base, word, i) ||
          (load ? !LoadWriteback(access, rt_rs)
                : (access->dest ||
                   !StoreSource(access->src3.value, rt_rs, i)))) {
        return CheckpointHIRResult::kInvalidOperands;
      }
    }
  }
  return ids.empty()
             ? CheckpointHIRResult::kNoPairs
             : CheckpointHIRResult::kStructuralPairRequiresFullProducerProof;
}

bool ContainsCheckpointHIR(const hir::HIRBuilder& builder) {
  for (auto block = builder.first_block(); block; block = block->next) {
    for (auto i = block->instr_head; i; i = i->next) {
      if (IsCheckpointOpcode(i->opcode->num)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace xe::cpu::ppc
