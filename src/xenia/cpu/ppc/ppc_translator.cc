/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_translator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string_view>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/reset_scope.h"
#include "xenia/base/string.h"
#include "xenia/cpu/compiler/compiler_passes.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_checkpoint_hir.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/ppc/ppc_hir_builder.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_scanner.h"
#include "xenia/cpu/private_delay_guest_source.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"

DEFINE_bool(dump_translated_hir_functions, false, "dumps translated hir",
            "CPU");

DEFINE_bool(disable_context_promotion, false,
            "Disables Context Promotion optimizations, this may be needed for "
            "some sports games, but will reduce performance.",
            "CPU");

DEFINE_bool(
    private_delay_guest_source_trace, false,
    "Private one-shot bounded PPC source observation for guest function "
    "829E6360; joins compiler-owned host source-map evidence.",
    "CPU");

namespace xe {
namespace cpu {
namespace ppc {

using xe::cpu::backend::Backend;
using xe::cpu::compiler::Compiler;
namespace passes = xe::cpu::compiler::passes;

namespace {
std::atomic<bool> guest_source_attempted{false};

struct GuestSourceCapture {
  std::array<uint32_t, private_delay_guest_source::kMaxWords> words{};
  uint32_t first = private_delay_guest_source::kSelectedFunction;
  size_t observed = 0;
  bool contiguous = true;
};

void ObserveGuestWord(void* context, uint32_t address, uint32_t code) {
  auto& capture = *static_cast<GuestSourceCapture*>(context);
  if (uint64_t(address) != uint64_t(capture.first) + capture.observed * 4) {
    capture.contiguous = false;
  }
  if (capture.observed < capture.words.size()) {
    capture.words[capture.observed] = code;
  }
  ++capture.observed;
}

// Only called on the selected compilation after the process-wide one-shot
// admission. The ordinary Translate path has no capture object or source copy.
bool EmitSelectedGuestSource(PPCHIRBuilder* builder, GuestFunction* function,
                             uint32_t emit_flags,
                             std::unique_ptr<FunctionDebugInfo>* debug_info) {
  GuestSourceCapture capture;
  if (!builder->Emit(function, emit_flags, ObserveGuestWord, &capture)) {
    XELOGI("XGUESTWORDS001 unavailable reason=emit_failed function={:08X}",
           function->address());
    return false;
  }
  const uint32_t end = function->end_address();
  const uint32_t first = function->address();
  if (end < first || (end - first) % 4 || !capture.contiguous ||
      capture.observed != size_t((end - first) / 4) + 1) {
    XELOGI(
        "XGUESTWORDS001 unavailable reason=range_or_contiguity "
        "function={:08X} end={:08X} observed={}",
        first, end, capture.observed);
    return true;
  }
  if (*debug_info && (*debug_info)->source_disasm()) {
    XELOGI(
        "XGUESTWORDS001 unavailable reason=source_disasm_occupied "
        "function={:08X}",
        first);
    return true;
  }
  const size_t count =
      std::min(capture.observed, private_delay_guest_source::kMaxWords);
  StringBuffer text;
  text.AppendFormat(
      "XGUESTWORDS001:f={:08X}:e={:08X}:a={:08X}:n={:02X}:p={:X}:c=1:w=", first,
      end, first, count,
      unsigned(capture.observed > private_delay_guest_source::kMaxWords));
  for (size_t i = 0; i < count; ++i) {
    text.AppendFormat("{:08X}", capture.words[i]);
  }
  if (text.length() > private_delay_guest_source::kMaxTextBytes) {
    XELOGI(
        "XGUESTWORDS001 unavailable reason=serialization_cap "
        "function={:08X}",
        first);
    return true;
  }
  private_delay_guest_source::Record parsed;
  if (!private_delay_guest_source::ParseRecord(
          std::string_view(text.buffer(), text.length()), end, &parsed)) {
    XELOGI(
        "XGUESTWORDS001 unavailable reason=serialization_invalid "
        "function={:08X}",
        first);
    return true;
  }
  if (!*debug_info) {
    debug_info->reset(new FunctionDebugInfo());
  }
  (*debug_info)->set_source_disasm(xe_strdup(text.buffer()));
  return true;
}

class CheckpointPairValidationPass final : public compiler::CompilerPass {
 public:
  bool Run(hir::HIRBuilder* builder) override {
    const auto result = VerifyCheckpointHIR(*builder);
    return result == CheckpointHIRResult::kNoPairs ||
           result ==
               CheckpointHIRResult::kStructuralPairRequiresFullProducerProof;
  }
};
}  // namespace

bool AddDormantCheckpointPasses(compiler::Compiler& compiler,
                                const backend::MachineInfo* machine_info) {
  if (!machine_info) {
    return false;
  }
  // This recipe is never selected by Translate. It establishes HIR structure,
  // not modeled-state materialization, native descriptors or executable code.
  compiler.AddPass(std::make_unique<CheckpointPairValidationPass>());
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(
      std::make_unique<passes::RegisterAllocationPass>(machine_info));
  compiler.AddPass(std::make_unique<passes::ValidationPass>());
  compiler.AddPass(std::make_unique<passes::FinalizationPass>());
  compiler.AddPass(std::make_unique<CheckpointPairValidationPass>());
  return true;
}

PPCTranslator::PPCTranslator(PPCFrontend* frontend)
    : frontend_(frontend),
      compilation_options_(frontend->compilation_options()) {
  if (compilation_options_.profile != CompilationProfile::kOrdinary) {
    return;
  }

  Backend* backend = frontend->processor()->backend();

  scanner_.reset(new PPCScanner(frontend));
  builder_.reset(new PPCHIRBuilder(frontend));
  compiler_.reset(new Compiler(frontend->processor()));
  assembler_ = backend->CreateAssembler();
  assembler_->Initialize();

  bool validate = cvars::validate_hir;

  // Merge blocks early. This will let us use more context in other passes.
  // The CFG is required for simplification and dirtied by it.
  compiler_->AddPass(std::make_unique<passes::ControlFlowAnalysisPass>());
  compiler_->AddPass(std::make_unique<passes::ControlFlowSimplificationPass>());

  // Passes are executed in the order they are added. Multiple of the same
  // pass type may be used.
  if (!cvars::disable_context_promotion) {
    if (validate) {
      compiler_->AddPass(std::make_unique<passes::ValidationPass>());
    }

    compiler_->AddPass(std::make_unique<passes::ContextPromotionPass>());

    if (validate) {
      compiler_->AddPass(std::make_unique<passes::ValidationPass>());
    }
  }
  // Grouped simplification + constant propagation.
  // Loops until no changes are made.
  auto sap = std::make_unique<passes::ConditionalGroupPass>();
  sap->AddPass(std::make_unique<passes::SimplificationPass>());
  if (validate) {
    sap->AddPass(std::make_unique<passes::ValidationPass>());
  }
  sap->AddPass(std::make_unique<passes::ConstantPropagationPass>());
  if (validate) {
    sap->AddPass(std::make_unique<passes::ValidationPass>());
  }
  compiler_->AddPass(std::move(sap));

  if (backend->machine_info()->supports_extended_load_store) {
    // Backend supports the advanced LOAD/STORE instructions.
    // These will save us a lot of HIR opcodes.
    compiler_->AddPass(
        std::make_unique<passes::MemorySequenceCombinationPass>());
    if (validate) {
      compiler_->AddPass(std::make_unique<passes::ValidationPass>());
    }
  }
  compiler_->AddPass(std::make_unique<passes::SimplificationPass>());
  if (validate) {
    compiler_->AddPass(std::make_unique<passes::ValidationPass>());
  }
  // compiler_->AddPass(std::make_unique<passes::DeadStoreEliminationPass>());
  // if (validate)
  // compiler_->AddPass(std::make_unique<passes::ValidationPass>());
  compiler_->AddPass(std::make_unique<passes::DeadCodeEliminationPass>());
  if (validate) {
    compiler_->AddPass(std::make_unique<passes::ValidationPass>());
  }

  //// Removes all unneeded variables. Try not to add new ones after this.
  // compiler_->AddPass(new passes::ValueReductionPass());
  // if (validate) compiler_->AddPass(new passes::ValidationPass());

  // Register allocation for the target backend.
  // Will modify the HIR to add loads/stores.
  // This should be the last pass before finalization, as after this all
  // registers are assigned and ready to be emitted.
  compiler_->AddPass(std::make_unique<passes::RegisterAllocationPass>(
      backend->machine_info()));
  if (validate) {
    compiler_->AddPass(std::make_unique<passes::ValidationPass>());
  }

  // Must come last. The HIR is not really HIR after this.
  compiler_->AddPass(std::make_unique<passes::FinalizationPass>());
}

PPCTranslator::~PPCTranslator() = default;

class HirBuilderScope {
  PPCHIRBuilder* builder_;

 public:
  HirBuilderScope(PPCHIRBuilder* builder) : builder_(builder) {
    builder_->MakeCurrent();
  }

  ~HirBuilderScope() {
    if (builder_) {
      builder_->RemoveCurrent();
    }
  }
};
void PPCTranslator::DumpHIR(GuestFunction* function, PPCHIRBuilder* builder) {
  if (cvars::dump_translated_hir_functions) {
    StringBuffer buffer{};
    builder_->Dump(&buffer);

    XexModule* mod = dynamic_cast<XexModule*>(function->module());

    std::wstring folder_name = L"hirdump";

    if (mod) {
      xex2_opt_execution_info* opt_exec_info = nullptr;
      if (mod->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &opt_exec_info)) {
        folder_name =
            L"hirdump_title_" + std::to_wstring(opt_exec_info->title_id);
      }
    }
    std::filesystem::path folder_path{folder_name};

    if (!std::filesystem::exists(folder_path)) {
      std::filesystem::create_directory(folder_path);
    }

    {
      wchar_t tmpbuf[64];
#ifdef XE_PLATFORM_WIN32
      _snwprintf(tmpbuf, 64, L"%X", function->address());
#else
      swprintf(tmpbuf, 64, L"%X", function->address());
#endif
      folder_path.append(&tmpbuf[0]);
    }

    FILE* f = fopen(folder_path.string().c_str(), "w");
    if (f) {
      fputs(buffer.buffer(), f);
      fclose(f);
    }
  }
}
bool PPCTranslator::Translate(GuestFunction* function,
                              uint32_t debug_info_flags) {
  // Direct translator calls must not bypass the unsupported profile gate.
  if (compilation_options_.profile != CompilationProfile::kOrdinary ||
      (function->compiled_profile() != CompilationProfile::kUncompiled &&
       function->compiled_profile() != compilation_options_.profile)) {
    return false;
  }

  SCOPE_profile_cpu_f("cpu");
  HirBuilderScope hir_build_scope{builder_.get()};
  // Reset() all caching when we leave.
  xe::make_reset_scope(builder_);
  xe::make_reset_scope(compiler_);
  xe::make_reset_scope(assembler_);
  xe::make_reset_scope(&string_buffer_);

  bool selected_source = false;
  if (cvars::private_delay_guest_source_trace &&
      function->address() == private_delay_guest_source::kSelectedFunction) {
    bool unclaimed = false;
    selected_source = guest_source_attempted.compare_exchange_strong(
        unclaimed, true, std::memory_order_acq_rel);
    if (selected_source &&
        ((debug_info_flags & DebugInfoFlags::kDebugInfoDisasmSource) ||
         cvars::disassemble_functions)) {
      XELOGI(
          "XGUESTWORDS001 unavailable reason=global_disassembly_conflict "
          "function={:08X}",
          function->address());
      selected_source = false;
    }
  }

  // NOTE: we only want to do this when required, as it's expensive to build.
  if (cvars::disassemble_functions) {
    debug_info_flags |= DebugInfoFlags::kDebugInfoAllDisasm;
  }
  if (cvars::trace_functions) {
    debug_info_flags |= DebugInfoFlags::kDebugInfoTraceFunctions;
  }
  if (cvars::trace_function_coverage) {
    debug_info_flags |= DebugInfoFlags::kDebugInfoTraceFunctionCoverage;
  }
  if (cvars::trace_function_references) {
    debug_info_flags |= DebugInfoFlags::kDebugInfoTraceFunctionReferences;
  }
  if (cvars::trace_function_data) {
    debug_info_flags |= DebugInfoFlags::kDebugInfoTraceFunctionData;
  }
  std::unique_ptr<FunctionDebugInfo> debug_info;
  if (debug_info_flags) {
    debug_info.reset(new FunctionDebugInfo());
  }

  // Scan the function to find its extents and gather debug data.
  if (!scanner_->Scan(function, debug_info.get())) {
    if (selected_source) {
      XELOGI("XGUESTWORDS001 unavailable reason=scan_failed function={:08X}",
             function->address());
    }
    return false;
  }

  // Setup trace data, if needed.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoTraceFunctions) {
    // Base trace data.
    size_t trace_data_size = FunctionTraceData::SizeOfHeader();
    if (debug_info_flags & DebugInfoFlags::kDebugInfoTraceFunctionCoverage) {
      // Additional space for instruction coverage counts.
      trace_data_size += FunctionTraceData::SizeOfInstructionCounts(
          function->address(), function->end_address());
    }
    uint8_t* trace_data =
        frontend_->processor()->AllocateFunctionTraceData(trace_data_size);
    if (trace_data) {
      function->trace_data().Reset(trace_data, trace_data_size,
                                   function->address(),
                                   function->end_address());
    } else {
      debug_info_flags &= ~(DebugInfoFlags::kDebugInfoTraceFunctions |
                            DebugInfoFlags::kDebugInfoTraceFunctionCoverage);
    }
  }

  // Stash source.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmSource) {
    DumpSource(function, &string_buffer_);
    debug_info->set_source_disasm(xe_strdup(string_buffer_.buffer()));
    string_buffer_.Reset();
  }

  // Emit function.
  uint32_t emit_flags = 0;
  if (debug_info) {
    emit_flags |= PPCHIRBuilder::EMIT_DEBUG_COMMENTS;
  }
  const bool emitted = selected_source
                           ? EmitSelectedGuestSource(builder_.get(), function,
                                                     emit_flags, &debug_info)
                           : builder_->Emit(function, emit_flags);
  if (!emitted) {
    return false;
  }

  // Stash raw HIR.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmRawHir) {
    builder_->Dump(&string_buffer_);
    debug_info->set_raw_hir_disasm(xe_strdup(string_buffer_.buffer()));
    string_buffer_.Reset();
  }

  // Compile/optimize/etc.
  if (!compiler_->Compile(builder_.get())) {
    return false;
  }

  // Stash optimized HIR.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmHir) {
    builder_->Dump(&string_buffer_);
    debug_info->set_hir_disasm(xe_strdup(string_buffer_.buffer()));
    string_buffer_.Reset();
  }

  DumpHIR(function, builder_.get());

  // Assemble to backend machine code.
  if (!assembler_->Assemble(function, builder_.get(), debug_info_flags,
                            std::move(debug_info))) {
    return false;
  }

  CompilationProfile expected = CompilationProfile::kUncompiled;
  if (!function->compiled_profile_.compare_exchange_strong(
          expected, compilation_options_.profile, std::memory_order_release,
          std::memory_order_relaxed) &&
      expected != compilation_options_.profile) {
    return false;
  }

  return true;
}
void PPCTranslator::Reset() {
  if (builder_) {
    builder_->ResetPools();
  }
}
void PPCTranslator::DumpSource(GuestFunction* function,
                               StringBuffer* string_buffer) {
  Memory* memory = frontend_->memory();

  string_buffer->AppendFormat(
      "{} fn {:08X}-{:08X} {}\n", function->module()->name().c_str(),
      function->address(), function->end_address(), function->name().c_str());

  auto blocks = scanner_->FindBlocks(function);

  uint32_t start_address = function->address();
  uint32_t end_address = function->end_address();
  auto block_it = blocks.begin();
  for (uint32_t address = start_address, offset = 0; address <= end_address;
       address += 4, offset++) {
    uint32_t code =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));

    // Check labels.
    if (block_it != blocks.end() && block_it->start_address == address) {
      string_buffer->AppendFormat("{:08X}          loc_{:08X}:\n", address,
                                  address);
      ++block_it;
    }

    string_buffer->AppendFormat("{:08X} {:08X}   ", address, code);
    DisasmPPC(address, code, string_buffer);
    string_buffer->Append('\n');
  }
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
