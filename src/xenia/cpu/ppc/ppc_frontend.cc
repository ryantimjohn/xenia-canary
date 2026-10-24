/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_frontend.h"

#include <chrono>

#include "xenia/base/atomic.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_emit.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#include "xenia/cpu/processor.h"

DEFINE_uint32(guest_gpr_capture_pc, 0,
              "Preinstruction capture PC; both PCs zero disable.", "CPU");
DEFINE_uint32(guest_gpr_observe_pc, 0, "Preinstruction observation PC.", "CPU");
DEFINE_uint32(guest_gpr_capture_key, 0,
              "Capture key GPR index (full 64-bit value).", "CPU");
DEFINE_uint32(guest_gpr_observe_key, 0,
              "Observe key GPR index (full 64-bit value).", "CPU");
DEFINE_uint32(guest_gpr_capture_mask, 0,
              "Capture GPR bitmask, at most six bits.", "CPU");
DEFINE_uint32(guest_gpr_observe_mask, 0,
              "Observe GPR bitmask, at most six bits.", "CPU");
DEFINE_int32(guest_gpr_predicate_gpr, -1,
             "Observe low32 equality GPR; -1 disables predicate.", "CPU");
DEFINE_uint32(guest_gpr_predicate_value, 0, "Observe predicate low32 value.",
              "CPU");
DEFINE_uint32(guest_gpr_delay_ms, 0,
              "Observation delay from first capture/observe checkpoint, steady "
              "wall time.",
              "CPU");

namespace xe {
namespace cpu {
namespace ppc {

void InitializeIfNeeded();
void CleanupOnShutdown();

void InitializeIfNeeded() {
  static bool has_initialized = false;
  if (has_initialized) {
    return;
  }
  has_initialized = true;

  RegisterEmitCategoryAltivec();
  RegisterEmitCategoryALU();
  RegisterEmitCategoryControl();
  RegisterEmitCategoryFPU();
  RegisterEmitCategoryMemory();

  atexit(CleanupOnShutdown);
}

void CleanupOnShutdown() {}

PPCFrontend::PPCFrontend(Processor* processor)
    : processor_(processor),
      compilation_options_(processor->compilation_options()) {
  if (compilation_options_.profile == CompilationProfile::kOrdinary) {
    InitializeIfNeeded();
  }
}

PPCFrontend::~PPCFrontend() {
  // Force cleanup now before we deinit.
  translator_pool_.Reset();
}

Memory* PPCFrontend::memory() const { return processor_->memory(); }

void PPCFrontend::EmitGuestGPRCheckpoint(hir::HIRBuilder& builder,
                                         uint32_t pc) {
  if (!guest_gpr_checkpoints_) {
    return;
  }
  for (const auto& point : guest_gpr_checkpoint_descriptors_) {
    if (point.pc == pc) {
      builder.ContextBarrier();
      builder.CallExtern(point.builtin);
      return;
    }
  }
}

void PPCFrontend::VisitGuestGPRCheckpoint(PPCContext* context, void* arg0,
                                          void*) {
  const auto& point = *static_cast<const CheckpointDescriptor*>(arg0);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  const auto row = point.collector->Visit(point.capture, context->thread_id,
                                          context->r, uint64_t(now));
  if (!row) {
    return;
  }
  const auto& config = point.collector->config();
  XELOGI(
      "Guest GPR checkpoint: seq={} pc={:08X} thread={:08X} key={:016X} "
      "steady_epoch_ms={} elapsed_ms={} matched={} cache_evictions={} "
      "capture_pc={:08X} capture_seq={} capture_age_ms={} "
      "observe_mask={:08X} "
      "observe=[{:016X},{:016X},{:016X},{:016X},{:016X},{:016X}] "
      "capture_mask={:08X} "
      "capture=[{:016X},{:016X},{:016X},{:016X},{:016X},{:016X}]",
      row->sequence, point.pc, row->thread, row->key, row->epoch_ms,
      row->elapsed_ms, row->matched, row->cache_evictions, config.capture_pc,
      row->capture_sequence, row->capture_age_ms, config.observe_mask,
      row->observed[0], row->observed[1], row->observed[2], row->observed[3],
      row->observed[4], row->observed[5], config.capture_mask, row->captured[0],
      row->captured[1], row->captured[2], row->captured[3], row->captured[4],
      row->captured[5]);
  if (row->cap_reached) {
    XELOGI(
        "Guest GPR checkpoint: 64 observation rows reserved; further rows "
        "suppressed for this frontend");
  }
}

// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  std::lock_guard<global_mutex_type> lock(*global_mutex);
  ppc_context->scratch = *global_lock_count ? 0 : 0x8000;
}

// Enters the global lock. Safe to recursion.
void EnterGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  global_mutex->lock();
  xe::atomic_inc(global_lock_count);
}

// Leaves the global lock. Safe to recursion.
void LeaveGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  auto new_lock_count = xe::atomic_dec(global_lock_count);
  assert_true(new_lock_count >= 0);
  global_mutex->unlock();
}

void SyscallHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  uint64_t syscall_number = ppc_context->r[0];
  switch (syscall_number) {
    default:
      assert_unhandled_case(syscall_number);
      XELOGE("Unhandled syscall {}!", syscall_number);
      break;
#pragma warning(suppress : 4065)
  }
}

bool PPCFrontend::Initialize() {
  if (compilation_options_.profile != CompilationProfile::kOrdinary) {
    return false;
  }

  void* arg0 = reinterpret_cast<void*>(&xe::global_critical_region::mutex());
  void* arg1 = reinterpret_cast<void*>(&builtins_.global_lock_count);
  builtins_.check_global_lock =
      processor_->DefineBuiltin("CheckGlobalLock", CheckGlobalLock, arg0, arg1);
  builtins_.enter_global_lock =
      processor_->DefineBuiltin("EnterGlobalLock", EnterGlobalLock, arg0, arg1);
  builtins_.leave_global_lock =
      processor_->DefineBuiltin("LeaveGlobalLock", LeaveGlobalLock, arg0, arg1);
  builtins_.syscall_handler = processor_->DefineBuiltin(
      "SyscallHandler", SyscallHandler, nullptr, nullptr);
  const GuestGPRCheckpointConfig checkpoint_config{
      cvars::guest_gpr_capture_pc,    cvars::guest_gpr_observe_pc,
      cvars::guest_gpr_capture_key,   cvars::guest_gpr_observe_key,
      cvars::guest_gpr_capture_mask,  cvars::guest_gpr_observe_mask,
      cvars::guest_gpr_predicate_gpr, cvars::guest_gpr_predicate_value,
      cvars::guest_gpr_delay_ms};
  if (const char* error = checkpoint_config.Validate()) {
    XELOGE("Guest GPR checkpoints disabled: {}", error);
  } else if (!checkpoint_config.disabled()) {
    guest_gpr_checkpoints_ =
        std::make_unique<GuestGPRCheckpointCollector>(checkpoint_config);
    for (size_t index = 0; index < 2; ++index) {
      auto& point = guest_gpr_checkpoint_descriptors_[index];
      point.collector = guest_gpr_checkpoints_.get();
      point.capture = index == 0;
      point.pc = point.capture ? checkpoint_config.capture_pc
                               : checkpoint_config.observe_pc;
      point.builtin =
          processor_->DefineBuiltin(point.capture ? "CaptureGuestGPRCheckpoint"
                                                  : "ObserveGuestGPRCheckpoint",
                                    VisitGuestGPRCheckpoint, &point, nullptr);
    }
  }
  return true;
}

bool PPCFrontend::DeclareFunction(GuestFunction* function) {
  if (compilation_options_.profile != CompilationProfile::kOrdinary) {
    return false;
  }

  // chrispy: make sure we aren't declaring a function that is actually padding
  // data, this will mess up PPCScanner and is hard to debug wow, this halo
  // reach actually has branches into 0 opcodes, look into further
  // xenia_assert(*reinterpret_cast<const uint32_t*>(
  //                  this->memory()->TranslateVirtual(function->address())) !=
  //                  0);
  //  Could scan or something here.
  //  Could also check to see if it's a well-known function type and classify
  //  for later.
  //  Could also kick off a precompiler, since we know it's likely the function
  //  will be demanded soon.
  return true;
}

bool PPCFrontend::DefineFunction(GuestFunction* function,
                                 uint32_t debug_info_flags) {
  if (compilation_options_.profile != CompilationProfile::kOrdinary) {
    return false;
  }

  auto translator = translator_pool_.Allocate(this);
  bool result = translator->Translate(function, debug_info_flags);
  translator->Reset();
  translator_pool_.Release(translator);
  return result;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
