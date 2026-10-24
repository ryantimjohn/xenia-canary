/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_MUTANT_STATE_H_
#define XENIA_KERNEL_MUTANT_STATE_H_

#include <cstdint>
#include <memory>
#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

struct X_KMUTANT;

// The host mutex enforces exclusion and recursion. This state records those
// acquisitions for guest-visible fields and the kernel's owner bookkeeping.
class MutantState {
 public:
  struct Ownership {
    uintptr_t owner;
    uint32_t depth;
  };

  bool Initialize(bool initial_owner, uintptr_t owner,
                  uint32_t guest_owner = 0);
  bool InitializeNative(X_KMUTANT* native, uintptr_t current_owner,
                        uint32_t current_guest_owner);
  void Acquired(uintptr_t owner, uint32_t guest_owner,
                X_KMUTANT* native = nullptr, bool abandoned = false);
  X_STATUS Release(uintptr_t owner, bool abandon, bool wait,
                   int32_t* previous_state, X_KMUTANT* native = nullptr);
  Ownership ownership() const;
  void SetRestoredOwner(uintptr_t owner);
  threading::WaitHandle* wait_handle() const { return mutant_.get(); }

 private:
  void UpdateNative(X_KMUTANT* native);

  std::unique_ptr<threading::Mutant> mutant_;
  mutable std::mutex state_lock_;
  uintptr_t owner_ = 0;
  uint64_t owner_host_thread_cookie_ = 0;
  uint32_t guest_owner_ = 0;
  uint32_t depth_ = 0;
};

bool InitializeNativeMutant(X_KMUTANT* native, bool initial_owner,
                            uint32_t guest_owner);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_MUTANT_STATE_H_
