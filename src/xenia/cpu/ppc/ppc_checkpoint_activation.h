/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_CHECKPOINT_ACTIVATION_H_
#define XENIA_CPU_PPC_PPC_CHECKPOINT_ACTIVATION_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "xenia/cpu/ppc/ppc_checkpoint_materialization.h"

namespace xe::cpu::ppc {

struct CheckpointActivationOwner {
  uint64_t owner_epoch = 0;
  uint64_t thread_epoch = 0;
};

struct CheckpointActivationToken {
  uint64_t arena_epoch = 0;
  CheckpointActivationOwner owner{};
  uint64_t generation = 0;
  uint32_t slot = UINT32_MAX;
};

struct CheckpointWriteToken {
  CheckpointActivationToken activation{};
  uint64_t sequence = 0;
};

enum class CheckpointActivationState { kInvalid, kWriting, kArmed, kDisarmed };
enum class CheckpointActivationResult {
  kSuccess,
  kInvalidOwner,
  kInvalidOutput,
  kExhausted,
  kCounterExhausted,
  kStaleToken,
  kNotCurrentActivation,
  kInvalidState,
  kIncompletePayload,
};

// Source/reference lifecycle only; kArmed means a complete reference payload is
// readable, NOT that a handler may consume it. There is no native descriptor,
// mapping lease, fault observation, code publication or runtime wiring here.
//
// The arena and all operations are exclusively owned by one host thread. Owner
// epochs are checked identity claims, not a thread-synchronization mechanism.
// The caller must supply nonzero, lifetime-unique arena/owner/thread epochs and
// keep the arena at a stable address. The arena cannot be copied or moved.
// Operations allocate no memory, take no locks and perform no logging. Reserve
// is intended outside a future native capture/fault window. No concurrent
// payload reader, seqlock guarantee, destructor cleanup or RAII exit is
// assumed.
//
// Reserve one record per host-to-guest activation. Direct guest calls reuse the
// current record. Nested reserve rejects Writing, and disarms an Armed parent
// so that returning from a nested activation cannot revive its old capture.
// Retire is LIFO; Abandon explicitly retires an activation and all its children
// for nonlocal cleanup. Runtime exit/reentry/tail/nonlocal integration is
// absent.
class CheckpointActivationArena {
 public:
  static constexpr size_t kCapacity = 8;

  CheckpointActivationArena(uint64_t arena_epoch,
                            CheckpointActivationOwner owner);
  CheckpointActivationArena(const CheckpointActivationArena&) = delete;
  CheckpointActivationArena& operator=(const CheckpointActivationArena&) =
      delete;
  CheckpointActivationArena(CheckpointActivationArena&&) = delete;
  CheckpointActivationArena& operator=(CheckpointActivationArena&&) = delete;

  CheckpointActivationResult Reserve(CheckpointActivationOwner owner,
                                     CheckpointActivationToken* output);
  CheckpointActivationResult BeginWrite(CheckpointActivationToken activation,
                                        CheckpointWriteToken* output);
  CheckpointActivationResult CaptureReference(CheckpointWriteToken write,
                                              const PPCContext* input);
  CheckpointActivationResult Arm(CheckpointWriteToken write);
  CheckpointActivationResult Disarm(CheckpointWriteToken write);
  CheckpointActivationResult CopyArmedState(CheckpointWriteToken write,
                                            FaultModeledState* output) const;
  CheckpointActivationResult Inspect(CheckpointActivationToken activation,
                                     CheckpointActivationState* output) const;
  CheckpointActivationResult Retire(CheckpointActivationToken activation);
  CheckpointActivationResult Abandon(CheckpointActivationToken activation);
  CheckpointActivationResult InvalidateAll(CheckpointActivationOwner owner);
  size_t depth() const { return depth_; }

 private:
  struct Record {
    FaultModeledState payload{};
    uint64_t generation = 0;
    uint64_t sequence = 0;
    CheckpointActivationState state = CheckpointActivationState::kInvalid;
    bool complete = false;
  };

  bool ValidOwner(CheckpointActivationOwner owner) const;
  CheckpointActivationResult ValidateActivation(
      CheckpointActivationToken activation, bool current) const;
  CheckpointActivationResult ValidateWrite(CheckpointWriteToken write) const;
  void InvalidateRecord(size_t slot);

  const uint64_t arena_epoch_;
  const CheckpointActivationOwner owner_;
  std::array<Record, kCapacity> records_{};
  size_t depth_ = 0;
  uint64_t generation_ = 0;
  uint64_t sequence_ = 0;
};

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_PPC_CHECKPOINT_ACTIVATION_H_
