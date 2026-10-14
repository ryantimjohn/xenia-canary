/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_activation.h"

#include <limits>

namespace xe::cpu::ppc {

CheckpointActivationArena::CheckpointActivationArena(
    uint64_t arena_epoch, CheckpointActivationOwner owner)
    : arena_epoch_(arena_epoch), owner_(owner) {}

bool CheckpointActivationArena::ValidOwner(
    CheckpointActivationOwner owner) const {
  return arena_epoch_ && owner_.owner_epoch && owner_.thread_epoch &&
         owner.owner_epoch == owner_.owner_epoch &&
         owner.thread_epoch == owner_.thread_epoch;
}

CheckpointActivationResult CheckpointActivationArena::ValidateActivation(
    CheckpointActivationToken activation, bool current) const {
  if (!ValidOwner(activation.owner)) {
    return CheckpointActivationResult::kInvalidOwner;
  }
  if (activation.arena_epoch != arena_epoch_ || !activation.generation ||
      activation.slot >= depth_ ||
      records_[activation.slot].generation != activation.generation) {
    return CheckpointActivationResult::kStaleToken;
  }
  if (current && activation.slot + 1 != depth_) {
    return CheckpointActivationResult::kNotCurrentActivation;
  }
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::ValidateWrite(
    CheckpointWriteToken write) const {
  const auto result = ValidateActivation(write.activation, true);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  if (!write.sequence ||
      records_[write.activation.slot].sequence != write.sequence) {
    return CheckpointActivationResult::kStaleToken;
  }
  return CheckpointActivationResult::kSuccess;
}

void CheckpointActivationArena::InvalidateRecord(size_t slot) {
  auto& record = records_[slot];
  record.state = CheckpointActivationState::kInvalid;
  record.complete = false;
  record.payload.modeled_valid = {};
  record.payload.architectural_valid = {};
  record.generation = 0;
  record.sequence = 0;
}

CheckpointActivationResult CheckpointActivationArena::Reserve(
    CheckpointActivationOwner owner, CheckpointActivationToken* output) {
  if (!output) {
    return CheckpointActivationResult::kInvalidOutput;
  }
  *output = {};
  if (!ValidOwner(owner)) {
    return CheckpointActivationResult::kInvalidOwner;
  }
  if (depth_ &&
      records_[depth_ - 1].state == CheckpointActivationState::kWriting) {
    return CheckpointActivationResult::kInvalidState;
  }
  if (depth_ == kCapacity) {
    return CheckpointActivationResult::kExhausted;
  }
  if (generation_ == std::numeric_limits<uint64_t>::max()) {
    return CheckpointActivationResult::kCounterExhausted;
  }
  if (depth_ &&
      records_[depth_ - 1].state == CheckpointActivationState::kArmed) {
    auto& parent = records_[depth_ - 1];
    parent.state = CheckpointActivationState::kDisarmed;
    parent.complete = false;
    parent.payload.modeled_valid = {};
  }
  const size_t slot = depth_;
  InvalidateRecord(slot);
  auto& record = records_[slot];
  record.generation = ++generation_;
  ++depth_;
  output->arena_epoch = arena_epoch_;
  output->owner = owner_;
  output->generation = record.generation;
  output->slot = static_cast<uint32_t>(slot);
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::BeginWrite(
    CheckpointActivationToken activation, CheckpointWriteToken* output) {
  if (!output) {
    return CheckpointActivationResult::kInvalidOutput;
  }
  *output = {};
  const auto result = ValidateActivation(activation, true);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  auto& record = records_[activation.slot];
  if (record.state != CheckpointActivationState::kInvalid &&
      record.state != CheckpointActivationState::kDisarmed) {
    return CheckpointActivationResult::kInvalidState;
  }
  if (sequence_ == std::numeric_limits<uint64_t>::max()) {
    return CheckpointActivationResult::kCounterExhausted;
  }
  record.state = CheckpointActivationState::kWriting;
  record.complete = false;
  record.payload.modeled_valid = {};
  record.payload.architectural_valid = {};
  record.sequence = ++sequence_;
  output->activation = activation;
  output->sequence = record.sequence;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::CaptureReference(
    CheckpointWriteToken write, const PPCContext* input) {
  const auto result = ValidateWrite(write);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  auto& record = records_[write.activation.slot];
  if (record.state != CheckpointActivationState::kWriting) {
    return CheckpointActivationResult::kInvalidState;
  }
  record.complete = false;
  if (!CopyModeledCheckpointStateReference(input, &record.payload)) {
    return CheckpointActivationResult::kIncompletePayload;
  }
  record.complete = true;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::Arm(
    CheckpointWriteToken write) {
  const auto result = ValidateWrite(write);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  auto& record = records_[write.activation.slot];
  if (record.state != CheckpointActivationState::kWriting) {
    return CheckpointActivationResult::kInvalidState;
  }
  if (!record.complete) {
    return CheckpointActivationResult::kIncompletePayload;
  }
  record.state = CheckpointActivationState::kArmed;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::Disarm(
    CheckpointWriteToken write) {
  const auto result = ValidateWrite(write);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  auto& record = records_[write.activation.slot];
  if (record.state != CheckpointActivationState::kWriting &&
      record.state != CheckpointActivationState::kArmed) {
    return CheckpointActivationResult::kInvalidState;
  }
  record.state = CheckpointActivationState::kDisarmed;
  record.complete = false;
  record.payload.modeled_valid = {};
  record.payload.architectural_valid = {};
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::CopyArmedState(
    CheckpointWriteToken write, FaultModeledState* output) const {
  if (!output) {
    return CheckpointActivationResult::kInvalidOutput;
  }
  output->modeled_valid = {};
  output->architectural_valid = {};
  const auto result = ValidateWrite(write);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  const auto& record = records_[write.activation.slot];
  if (record.state != CheckpointActivationState::kArmed) {
    return CheckpointActivationResult::kInvalidState;
  }
  if (!record.complete) {
    return CheckpointActivationResult::kIncompletePayload;
  }
  *output = record.payload;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::Inspect(
    CheckpointActivationToken activation,
    CheckpointActivationState* output) const {
  if (!output) {
    return CheckpointActivationResult::kInvalidOutput;
  }
  *output = CheckpointActivationState::kInvalid;
  const auto result = ValidateActivation(activation, false);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  *output = records_[activation.slot].state;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::Retire(
    CheckpointActivationToken activation) {
  const auto result = ValidateActivation(activation, true);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  InvalidateRecord(activation.slot);
  --depth_;
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::Abandon(
    CheckpointActivationToken activation) {
  const auto result = ValidateActivation(activation, false);
  if (result != CheckpointActivationResult::kSuccess) {
    return result;
  }
  while (depth_ > activation.slot) {
    InvalidateRecord(--depth_);
  }
  return CheckpointActivationResult::kSuccess;
}

CheckpointActivationResult CheckpointActivationArena::InvalidateAll(
    CheckpointActivationOwner owner) {
  if (!ValidOwner(owner)) {
    return CheckpointActivationResult::kInvalidOwner;
  }
  while (depth_) {
    InvalidateRecord(--depth_);
  }
  return CheckpointActivationResult::kSuccess;
}

}  // namespace xe::cpu::ppc
