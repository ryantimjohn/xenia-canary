/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xmutant.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_range.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XMutant::XMutant(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() = default;

void XMutant::Initialize(bool initial_owner) {
  auto* owner = initial_owner ? XThread::GetCurrentThread() : nullptr;
  if (!state_.Initialize(initial_owner, reinterpret_cast<uintptr_t>(owner),
                         owner ? owner->guest_object() : 0)) {
    xe::FatalError("Failed to initialize a mutant");
  }
}

void XMutant::InitializeNative(void* native_ptr,
                               const X_DISPATCH_HEADER* header) {
  const uint32_t guest_ptr = memory()->HostToGuestVirtual(native_ptr);
  if (!util::IsGuestRangeWritable(memory(), guest_ptr, sizeof(X_KMUTANT))) {
    xe::FatalError("Native mutant storage is not writable");
  }
  auto* current = XThread::GetCurrentThread();
  if (!state_.InitializeNative(
          memory()->TranslateVirtual<X_KMUTANT*>(guest_ptr),
          reinterpret_cast<uintptr_t>(current), current->guest_object())) {
    xe::FatalError(
        "Cannot wrap an abandoned, recursively owned, or foreign-owned "
        "native mutant");
  }
  SetNativePointer(guest_ptr, true);
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon,
                                bool wait, int32_t* previous_state) {
  if (abandon || wait) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  X_KMUTANT* native = nullptr;
  if (guest_object()) {
    if (!util::IsGuestRangeWritable(memory(), guest_object(),
                                    sizeof(X_KMUTANT))) {
      return X_STATUS_ACCESS_VIOLATION;
    }
    native = guest_object<X_KMUTANT>();
  }
  set_priority_increment(priority_increment);
  return state_.Release(
      reinterpret_cast<uintptr_t>(XThread::GetCurrentThread()), abandon, wait,
      previous_state, native);
}

bool XMutant::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  const auto ownership = state_.ownership();
  if (ownership.depth > 1) {
    // Save(false) is swallowed by KernelState, so it cannot safely reject
    // this snapshot here. Keep the historical stream layout and report the
    // limitation until save failures propagate to the caller.
    XELOGW("XMutant {:08X}: snapshot cannot preserve recursive depth {}",
           handle(), ownership.depth);
  }
  auto* owner = reinterpret_cast<XThread*>(ownership.owner);
  uint32_t owning_thread_handle = owner ? owner->handle() : 0;
  stream->Write<uint32_t>(owning_thread_handle);
  XELOGD("XMutant {:08X} (owner: {:08X})", handle(), owning_thread_handle);

  return true;
}

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto mutant = new XMutant();
  mutant->kernel_state_ = kernel_state;

  if (!mutant->RestoreObject(stream)) {
    delete mutant;
    return nullptr;
  }

  mutant->Initialize(false);

  auto owning_thread_handle = stream->Read<uint32_t>();
  if (owning_thread_handle) {
    auto owner = kernel_state->object_table()->LookupObject<XThread>(
        owning_thread_handle);
    if (!owner) {
      XELOGE("XMutant restore: owner {:08X} was not restored",
             owning_thread_handle);
      // Restored handles retain the object until the failed restore is reset.
      mutant->Release();
      return nullptr;
    }
    mutant->state_.SetRestoredOwner(reinterpret_cast<uintptr_t>(owner.get()));
    owner->AcquireMutantOnStartup(retain_object(mutant));
  }

  return object_ref<XMutant>(mutant);
}

void XMutant::WaitCallback() { RecordAcquisition(false); }

void XMutant::AbandonedWaitCallback() { RecordAcquisition(true); }

void XMutant::RecordAcquisition(bool abandoned) {
  auto* current = XThread::GetCurrentThread();
  X_KMUTANT* native = nullptr;
  if (guest_object()) {
    if (!util::IsGuestRangeWritable(memory(), guest_object(),
                                    sizeof(X_KMUTANT))) {
      xe::FatalError("Acquired native mutant storage is not writable");
    }
    native = guest_object<X_KMUTANT>();
  }
  state_.Acquired(reinterpret_cast<uintptr_t>(current), current->guest_object(),
                  native, abandoned);
}

}  // namespace kernel
}  // namespace xe
