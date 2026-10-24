/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_checkpoint_activation.h"

#include <array>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using Result = CheckpointActivationResult;
using State = CheckpointActivationState;
constexpr CheckpointActivationOwner kOwner{11, 17};

static_assert(!std::is_copy_constructible_v<CheckpointActivationArena>);
static_assert(!std::is_move_constructible_v<CheckpointActivationArena>);

TEST_CASE("Reference activation gates payload reads around a complete write",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken activation;
  REQUIRE(arena.Reserve(kOwner, &activation) == Result::kSuccess);
  State state = State::kArmed;
  REQUIRE(arena.Inspect(activation, &state) == Result::kSuccess);
  CHECK(state == State::kInvalid);
  FaultModeledState copy{};
  CHECK(arena.CopyArmedState({activation, 1}, &copy) == Result::kStaleToken);

  CheckpointWriteToken write;
  REQUIRE(arena.BeginWrite(activation, &write) == Result::kSuccess);
  CHECK(arena.Arm(write) == Result::kIncompletePayload);
  CHECK(arena.CopyArmedState(write, &copy) == Result::kInvalidState);
  PPCContext input{};
  input.r[31] = 0xABCDEF1234567890ULL;
  REQUIRE(arena.CaptureReference(write, &input) == Result::kSuccess);
  // Complete private payload still cannot be read until the lifecycle commits.
  CHECK(arena.CopyArmedState(write, &copy) == Result::kInvalidState);
  REQUIRE(arena.Arm(write) == Result::kSuccess);
  CHECK(arena.CaptureReference(write, &input) == Result::kInvalidState);
  REQUIRE(arena.CopyArmedState(write, &copy) == Result::kSuccess);
  CHECK(copy.gpr[31] == 0xABCDEF1234567890ULL);
  CHECK(copy.modeled_valid.gpr == UINT32_MAX);
  CHECK(copy.architectural_valid.gpr == 0);
  CHECK(copy.architectural_valid.scalar == 0);
  input.r[31] = 0;
  REQUIRE(arena.CopyArmedState(write, &copy) == Result::kSuccess);
  CHECK(copy.gpr[31] == 0xABCDEF1234567890ULL);

  REQUIRE(arena.Disarm(write) == Result::kSuccess);
  CHECK(arena.CopyArmedState(write, &copy) == Result::kInvalidState);
  CHECK(copy.modeled_valid.gpr == 0);
  REQUIRE(arena.Retire(activation) == Result::kSuccess);
  CHECK(arena.depth() == 0);
  CHECK(arena.CopyArmedState(write, &copy) == Result::kStaleToken);
}

TEST_CASE("Reference activation invalidates a failed replacement capture",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken activation;
  CheckpointWriteToken write;
  PPCContext input{};
  REQUIRE(arena.Reserve(kOwner, &activation) == Result::kSuccess);
  REQUIRE(arena.BeginWrite(activation, &write) == Result::kSuccess);
  REQUIRE(arena.CaptureReference(write, &input) == Result::kSuccess);
  CHECK(arena.CaptureReference(write, nullptr) == Result::kIncompletePayload);
  CHECK(arena.Arm(write) == Result::kIncompletePayload);
  FaultModeledState output{};
  CHECK(arena.CopyArmedState(write, &output) == Result::kInvalidState);
  REQUIRE(arena.Disarm(write) == Result::kSuccess);
  CHECK(arena.Arm(write) == Result::kInvalidState);
}

TEST_CASE("Reference activation rejects previous sequences and reused slots",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken first;
  CheckpointWriteToken old_write;
  REQUIRE(arena.Reserve(kOwner, &first) == Result::kSuccess);
  REQUIRE(arena.BeginWrite(first, &old_write) == Result::kSuccess);
  REQUIRE(arena.Disarm(old_write) == Result::kSuccess);
  CheckpointWriteToken next_write;
  REQUIRE(arena.BeginWrite(first, &next_write) == Result::kSuccess);
  CHECK(next_write.sequence > old_write.sequence);
  CHECK(arena.Disarm(old_write) == Result::kStaleToken);
  CHECK(arena.Arm(old_write) == Result::kStaleToken);
  REQUIRE(arena.Retire(first) == Result::kSuccess);
  CheckpointActivationToken second;
  REQUIRE(arena.Reserve(kOwner, &second) == Result::kSuccess);
  CHECK(second.slot == first.slot);
  CHECK(second.generation > first.generation);
  CHECK(arena.BeginWrite(first, &next_write) == Result::kStaleToken);
  CHECK(arena.Abandon(first) == Result::kStaleToken);
  REQUIRE(arena.BeginWrite(second, &next_write) == Result::kSuccess);
  CHECK(next_write.sequence > old_write.sequence);
}

TEST_CASE("Reference activation nesting rejects writes and retires in order",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken parent;
  CheckpointActivationToken child;
  CheckpointWriteToken write;
  PPCContext input{};
  input.ctr = 0x123456789ABCDEF0ULL;
  REQUIRE(arena.Reserve(kOwner, &parent) == Result::kSuccess);
  REQUIRE(arena.BeginWrite(parent, &write) == Result::kSuccess);
  REQUIRE(arena.CaptureReference(write, &input) == Result::kSuccess);
  CHECK(arena.Reserve(kOwner, &child) == Result::kInvalidState);
  CHECK(arena.depth() == 1);
  CHECK(child.generation == 0);
  State state = State::kInvalid;
  REQUIRE(arena.Inspect(parent, &state) == Result::kSuccess);
  CHECK(state == State::kWriting);
  // Failed nesting leaves the original sequence and complete payload intact.
  REQUIRE(arena.Arm(write) == Result::kSuccess);
  FaultModeledState copy{};
  REQUIRE(arena.CopyArmedState(write, &copy) == Result::kSuccess);
  CHECK(copy.ctr == 0x123456789ABCDEF0ULL);
  REQUIRE(arena.Disarm(write) == Result::kSuccess);
  REQUIRE(arena.Reserve(kOwner, &child) == Result::kSuccess);
  CHECK(child.slot != parent.slot);
  CHECK(child.generation != parent.generation);
  CHECK(arena.Retire(parent) == Result::kNotCurrentActivation);
  CHECK(arena.BeginWrite(parent, &write) == Result::kNotCurrentActivation);
  REQUIRE(arena.Retire(child) == Result::kSuccess);
  REQUIRE(arena.BeginWrite(parent, &write) == Result::kSuccess);
}

TEST_CASE("Nested reference activation cannot revive an Armed parent",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken parent;
  CheckpointWriteToken parent_write;
  PPCContext input{};
  REQUIRE(arena.Reserve(kOwner, &parent) == Result::kSuccess);
  REQUIRE(arena.BeginWrite(parent, &parent_write) == Result::kSuccess);
  REQUIRE(arena.CaptureReference(parent_write, &input) == Result::kSuccess);
  REQUIRE(arena.Arm(parent_write) == Result::kSuccess);
  CheckpointActivationToken child;
  REQUIRE(arena.Reserve(kOwner, &child) == Result::kSuccess);
  State state = State::kArmed;
  REQUIRE(arena.Inspect(parent, &state) == Result::kSuccess);
  CHECK(state == State::kDisarmed);
  FaultModeledState output{};
  CHECK(arena.CopyArmedState(parent_write, &output) ==
        Result::kNotCurrentActivation);
  REQUIRE(arena.Retire(child) == Result::kSuccess);
  CHECK(arena.CopyArmedState(parent_write, &output) == Result::kInvalidState);
  CHECK(arena.Arm(parent_write) == Result::kInvalidState);
}

TEST_CASE("Reference activation arena exhaustion preserves existing records",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  std::array<CheckpointActivationToken, CheckpointActivationArena::kCapacity>
      activations{};
  for (auto& activation : activations) {
    REQUIRE(arena.Reserve(kOwner, &activation) == Result::kSuccess);
  }
  CheckpointWriteToken write;
  PPCContext input{};
  input.lr = 0x9876543210ABCDEFULL;
  REQUIRE(arena.BeginWrite(activations.back(), &write) == Result::kSuccess);
  REQUIRE(arena.CaptureReference(write, &input) == Result::kSuccess);
  REQUIRE(arena.Arm(write) == Result::kSuccess);
  CheckpointActivationToken extra;
  CHECK(arena.Reserve(kOwner, &extra) == Result::kExhausted);
  CHECK(extra.generation == 0);
  CHECK(arena.depth() == CheckpointActivationArena::kCapacity);
  for (size_t n = 0; n < activations.size(); ++n) {
    State state;
    CHECK(arena.Inspect(activations[n], &state) == Result::kSuccess);
    CHECK(state ==
          (n + 1 == activations.size() ? State::kArmed : State::kInvalid));
  }
  FaultModeledState copy{};
  REQUIRE(arena.CopyArmedState(write, &copy) == Result::kSuccess);
  CHECK(copy.lr == 0x9876543210ABCDEFULL);
  REQUIRE(arena.Retire(activations.back()) == Result::kSuccess);
  REQUIRE(arena.Reserve(kOwner, &extra) == Result::kSuccess);
  CHECK(extra.generation > activations.back().generation);
  CHECK(arena.Retire(activations.back()) == Result::kStaleToken);
}

TEST_CASE("Reference activation abandonment invalidates all younger records",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationToken parent;
  CheckpointActivationToken child;
  CheckpointActivationToken grandchild;
  REQUIRE(arena.Reserve(kOwner, &parent) == Result::kSuccess);
  REQUIRE(arena.Reserve(kOwner, &child) == Result::kSuccess);
  REQUIRE(arena.Reserve(kOwner, &grandchild) == Result::kSuccess);
  CheckpointWriteToken write;
  REQUIRE(arena.BeginWrite(grandchild, &write) == Result::kSuccess);
  REQUIRE(arena.Abandon(child) == Result::kSuccess);
  CHECK(arena.depth() == 1);
  CHECK(arena.Arm(write) == Result::kStaleToken);
  CHECK(arena.Retire(child) == Result::kStaleToken);
  CHECK(arena.Retire(grandchild) == Result::kStaleToken);
  REQUIRE(arena.InvalidateAll(kOwner) == Result::kSuccess);
  CHECK(arena.depth() == 0);
  CHECK(arena.Retire(parent) == Result::kStaleToken);
  CheckpointActivationToken next;
  REQUIRE(arena.Reserve(kOwner, &next) == Result::kSuccess);
  CHECK(next.generation > grandchild.generation);
}

TEST_CASE("Reference activation rejects owner and arena identity mismatches",
          "[cpu][checkpoint-activation]") {
  CheckpointActivationArena arena(23, kOwner);
  CheckpointActivationArena other_arena(29, kOwner);
  CheckpointActivationToken activation;
  CheckpointActivationToken other;
  REQUIRE(arena.Reserve(kOwner, &activation) == Result::kSuccess);
  REQUIRE(other_arena.Reserve(kOwner, &other) == Result::kSuccess);
  CheckpointWriteToken write;
  CHECK(arena.BeginWrite(other, &write) == Result::kStaleToken);
  auto wrong_owner = activation;
  ++wrong_owner.owner.owner_epoch;
  CHECK(arena.BeginWrite(wrong_owner, &write) == Result::kInvalidOwner);
  wrong_owner = activation;
  ++wrong_owner.owner.thread_epoch;
  CHECK(arena.Abandon(wrong_owner) == Result::kInvalidOwner);
  CHECK(arena.InvalidateAll({11, 18}) == Result::kInvalidOwner);
  CHECK(arena.depth() == 1);
  CHECK(arena.Reserve({0, 17}, &other) == Result::kInvalidOwner);
  CHECK(arena.Reserve(kOwner, nullptr) == Result::kInvalidOutput);
  CHECK(arena.BeginWrite(activation, nullptr) == Result::kInvalidOutput);
  CHECK(arena.CopyArmedState({}, nullptr) == Result::kInvalidOutput);
  CHECK(arena.Inspect(activation, nullptr) == Result::kInvalidOutput);
  CheckpointActivationArena invalid_arena(0, kOwner);
  CHECK(invalid_arena.Reserve(kOwner, &other) == Result::kInvalidOwner);
}

}  // namespace
}  // namespace xe::cpu::ppc::test
