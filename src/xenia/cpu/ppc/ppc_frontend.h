/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_FRONTEND_H_
#define XENIA_CPU_PPC_PPC_FRONTEND_H_

#include <memory>

#include "xenia/base/type_pool.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/guest_gpr_checkpoint.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
class Processor;
namespace hir {
class HIRBuilder;
}
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace ppc {

class PPCTranslator;

struct PPCBuiltins {
  int32_t global_lock_count;
  Function* check_global_lock;
  Function* enter_global_lock;
  Function* leave_global_lock;
  Function* syscall_handler;
};

class PPCFrontend {
 public:
  explicit PPCFrontend(Processor* processor);
  ~PPCFrontend();

  bool Initialize();

  Processor* processor() const { return processor_; }
  CompilationOptions compilation_options() const {
    return compilation_options_;
  }
  Memory* memory() const;
  PPCBuiltins* builtins() { return &builtins_; }
  void EmitGuestGPRCheckpoint(hir::HIRBuilder& builder, uint32_t pc);
  const GuestGPRCheckpointCollector* guest_gpr_checkpoints() const {
    return guest_gpr_checkpoints_.get();
  }

  bool DeclareFunction(GuestFunction* function);
  bool DefineFunction(GuestFunction* function, uint32_t debug_info_flags);

 private:
  Processor* processor_;
  const CompilationOptions compilation_options_;
  PPCBuiltins builtins_ = {0};
  // Descriptors and collector outlive translator use and retain stable
  // addresses for builtin calls embedded in compiled code, for this frontend
  // lifetime.
  struct CheckpointDescriptor {
    GuestGPRCheckpointCollector* collector = nullptr;
    uint32_t pc = 0;
    bool capture = false;
    Function* builtin = nullptr;
  };
  static void VisitGuestGPRCheckpoint(PPCContext*, void*, void*);
  TypePool<PPCTranslator, PPCFrontend*> translator_pool_;
  // Append owned state so existing member offsets remain unchanged. Callers
  // allocating PPCFrontend (including Processor::Setup) must be rebuilt.
  std::unique_ptr<GuestGPRCheckpointCollector> guest_gpr_checkpoints_;
  std::array<CheckpointDescriptor, 2> guest_gpr_checkpoint_descriptors_{};
};
// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1);
}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_FRONTEND_H_
