/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <memory>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#include "xenia/cpu/raw_module.h"
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

namespace xe::cpu::ppc::test {

TEST_CASE("Processor owns options and refuses checkpoint before setup",
          "[cpu][compilation-profile]") {
  CompilationOptions options{CompilationProfile::kCheckpoint};
  Processor processor(nullptr, nullptr, options);
  options.profile = CompilationProfile::kOrdinary;
  auto returned_options = processor.compilation_options();
  returned_options.profile = CompilationProfile::kUncompiled;
  CHECK(returned_options.profile == CompilationProfile::kUncompiled);

  CHECK(processor.compilation_options().profile ==
        CompilationProfile::kCheckpoint);
  // No memory or backend is available: reaching ordinary setup is an error.
  CHECK_FALSE(processor.Setup(nullptr));
  CHECK(processor.frontend() == nullptr);
  CHECK(processor.backend() == nullptr);
  CHECK(processor.builtin_module() == nullptr);
  CHECK(processor.GetModules().empty());
}

TEST_CASE("Direct frontend and translator calls refuse checkpoint",
          "[cpu][compilation-profile]") {
  Processor processor(nullptr, nullptr,
                      CompilationOptions{CompilationProfile::kCheckpoint});
  PPCFrontend frontend(&processor);
  auto returned_options = frontend.compilation_options();
  returned_options.profile = CompilationProfile::kOrdinary;
  CHECK(returned_options.profile == CompilationProfile::kOrdinary);
  CHECK(frontend.compilation_options().profile ==
        CompilationProfile::kCheckpoint);

  const auto current_hir = hir::HIRBuilder::GetCurrent();
  CHECK_FALSE(frontend.Initialize());
  CHECK_FALSE(frontend.DeclareFunction(nullptr));
  CHECK_FALSE(frontend.DefineFunction(nullptr, 0));
  PPCTranslator translator(&frontend);
  CHECK_FALSE(translator.Translate(nullptr, 0));
  translator.Reset();

  CHECK(hir::HIRBuilder::GetCurrent() == current_hir);
  CHECK(frontend.builtins()->check_global_lock == nullptr);
  CHECK(frontend.builtins()->enter_global_lock == nullptr);
  CHECK(frontend.builtins()->leave_global_lock == nullptr);
  CHECK(frontend.builtins()->syscall_handler == nullptr);
  CHECK(processor.frontend() == nullptr);
  CHECK(processor.backend() == nullptr);
  CHECK(processor.GetModules().empty());
}

TEST_CASE("Default ordinary translation stamps completed guest function",
          "[cpu][compilation-profile]") {
  // Memory outlives the processor, which owns its module and native code.
  Memory memory;
  REQUIRE(memory.Initialize());
  std::unique_ptr<backend::Backend> native_backend;
#if XE_ARCH_AMD64
  native_backend = std::make_unique<backend::x64::X64Backend>();
#elif XE_ARCH_ARM64
  native_backend = std::make_unique<backend::a64::A64Backend>();
#endif
  REQUIRE(native_backend);
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::move(native_backend)));
  CHECK(processor.compilation_options().profile ==
        CompilationProfile::kOrdinary);
  CHECK(processor.frontend()->compilation_options().profile ==
        CompilationProfile::kOrdinary);

  constexpr uint32_t address = 0x82000000;
  auto* heap = memory.LookupHeap(address);
  REQUIRE(heap);
  REQUIRE(heap->AllocFixed(address, 0x1000, 0x1000,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(address), 0x4E800020);
  auto module = std::make_unique<RawModule>(&processor);
  module->set_name("CompilationProfileTest");
  module->set_executable(true);
  module->SetAddressRange(address, 4);
  REQUIRE(processor.AddModule(std::move(module)));

  auto* declared = processor.LookupFunction(address);
  REQUIRE(declared);
  REQUIRE(declared->is_guest());
  auto* guest = static_cast<GuestFunction*>(declared);
  CHECK(guest->compiled_profile() == CompilationProfile::kUncompiled);

  // Resolve compiles real PPC through PPCTranslator; it never calls the guest.
  REQUIRE(processor.ResolveFunction(address) == guest);
  CHECK(guest->compiled_profile() == CompilationProfile::kOrdinary);
  CHECK(guest->machine_code() != nullptr);
  CHECK(guest->machine_code_length() > 0);
  CHECK(processor.ResolveFunction(address) == guest);
  CHECK(guest->compiled_profile() == CompilationProfile::kOrdinary);
}

}  // namespace xe::cpu::ppc::test
