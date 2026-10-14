/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/mutant_state.h"

#include <atomic>
#include <cstring>
#include <limits>

#include "xenia/base/logging.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

namespace {

uint64_t CurrentHostThreadCookie() {
  static std::atomic<uint64_t> next_cookie{1};
  thread_local const uint64_t cookie = [] {
    uint64_t next = next_cookie.load(std::memory_order_relaxed);
    for (;;) {
      if (next == std::numeric_limits<uint64_t>::max()) {
        xe::FatalError("Mutant host thread lifetime cookies exhausted");
      }
      if (next_cookie.compare_exchange_weak(next, next + 1,
                                            std::memory_order_relaxed)) {
        return next;
      }
    }
  }();
  return cookie;
}

}  // namespace

bool InitializeNativeMutant(X_KMUTANT* native, bool initial_owner,
                            uint32_t guest_owner) {
  // Reinitialization must not detach a live wrapper that still owns the
  // object's host mutex and may update this same guest storage.
  if (native->header.wait_list.flink_ptr == kXObjSignature) {
    return false;
  }
  std::memset(native, 0, sizeof(*native));
  native->header.type = MutantObject;
  native->header.signal_state = initial_owner ? 0 : 1;
  native->owner = initial_owner ? guest_owner : 0;
  return true;
}

bool MutantState::Initialize(bool initial_owner, uintptr_t owner,
                             uint32_t guest_owner) {
  assert_false(mutant_);
  if (initial_owner && !owner) {
    return false;
  }
  mutant_ = threading::Mutant::Create(initial_owner);
  if (!mutant_) {
    return false;
  }
  owner_ = initial_owner ? owner : 0;
  owner_host_thread_cookie_ = initial_owner ? CurrentHostThreadCookie() : 0;
  guest_owner_ = initial_owner ? guest_owner : 0;
  depth_ = initial_owner ? 1 : 0;
  return true;
}

bool MutantState::InitializeNative(X_KMUTANT* native, uintptr_t current_owner,
                                   uint32_t current_guest_owner) {
  // A late wrapper may not transfer an already owned guest mutant to the
  // caller. Only an unowned object or the initializing owner is supported.
  const uint32_t signal_state = native->header.signal_state;
  const bool initial_owner = signal_state == 0;
  if (native->header.type != MutantObject || signal_state > 1 ||
      native->abandoned || (!initial_owner && native->owner != 0) ||
      (initial_owner &&
       (!current_guest_owner || native->owner != current_guest_owner))) {
    return false;
  }
  if (!Initialize(initial_owner, current_owner, current_guest_owner)) {
    return false;
  }
  UpdateNative(native);
  return true;
}

void MutantState::Acquired(uintptr_t owner, uint32_t guest_owner,
                           X_KMUTANT* native, bool abandoned) {
  std::lock_guard<std::mutex> lock(state_lock_);
  assert_not_zero(owner);
  const uint64_t host_cookie = CurrentHostThreadCookie();
  // A guest owner pointer can be reused after thread destruction. Only the
  // same real host thread may retain recursive acquisitions; a known
  // abandoned acquisition always starts a new ownership with depth 1.
  if (abandoned || owner_ != owner ||
      owner_host_thread_cookie_ != host_cookie) {
    depth_ = 0;
  }
  if (depth_ == static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    xe::FatalError("Mutant recursive ownership exceeds the supported limit");
  }
  owner_ = owner;
  owner_host_thread_cookie_ = host_cookie;
  guest_owner_ = guest_owner;
  ++depth_;
  UpdateNative(native);
}

X_STATUS MutantState::Release(uintptr_t owner, bool abandon, bool wait,
                              int32_t* previous_state, X_KMUTANT* native) {
  std::lock_guard<std::mutex> lock(state_lock_);
  // Abandoning and the Ke wait-next scheduler protocol need separate work.
  if (abandon || wait) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (!depth_ || owner_ != owner ||
      owner_host_thread_cookie_ != CurrentHostThreadCookie()) {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  const int32_t previous = static_cast<int32_t>(1u - depth_);
  // Keep the lock across the host release and metadata update: a woken
  // thread's Acquired must not be overwritten by this owner's final release.
  if (!mutant_->Release()) {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  if (!--depth_) {
    owner_ = 0;
    owner_host_thread_cookie_ = 0;
    guest_owner_ = 0;
  }
  UpdateNative(native);
  if (previous_state) {
    *previous_state = previous;
  }
  return X_STATUS_SUCCESS;
}

MutantState::Ownership MutantState::ownership() const {
  std::lock_guard<std::mutex> lock(state_lock_);
  return {owner_, depth_};
}

void MutantState::SetRestoredOwner(uintptr_t owner) {
  std::lock_guard<std::mutex> lock(state_lock_);
  owner_ = owner;
  owner_host_thread_cookie_ = 0;
  // The existing restore startup queues one host acquisition. Its callback
  // publishes depth 1; the old snapshot stream contains no recursive depth.
  depth_ = 0;
}

void MutantState::UpdateNative(X_KMUTANT* native) {
  if (native) {
    native->header.signal_state = 1 - depth_;
    native->owner = guest_owner_;
    native->abandoned = false;
  }
}

}  // namespace kernel
}  // namespace xe
