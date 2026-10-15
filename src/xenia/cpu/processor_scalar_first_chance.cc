/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"

DEFINE_bool(zero_page_scalar_first_chance, false,
            "Experimental bounded search-only callbacks before literal RA=0 "
            "protected-zero LWZ/STW; no dynamic addresses or continuation.",
            "CPU");
DECLARE_bool(protect_zero);

namespace xe::cpu {
namespace {

// Memory::Initialize reserves this guard with no access when protect_zero is
// enabled. Guest protection can change later; eligibility also checks the
// current guest protection. Neither check prevents a concurrent mapping change.
constexpr uint32_t kZeroGuardEnd = 0x10000;
[[maybe_unused]] constexpr size_t kMaxFirstChanceSites = 8192;
// Conservative: every nested offer on this host thread declines, including
// offers involving a different guest ThreadState during a callback.
thread_local bool scalar_access_first_chance_active = false;

uint32_t LiteralAddress(uint32_t word) {
  const uint32_t low = word & 0xFFFF;
  return (low & 0x8000) ? low | 0xFFFF0000u : low;
}

[[maybe_unused]] bool IsSupportedWord(uint32_t word) {
  return ((word >> 26) == 32 || (word >> 26) == 36) &&
         ((word >> 16) & 31) == 0 && LiteralAddress(word) <= kZeroGuardEnd - 4;
}

}  // namespace

void Processor::set_scalar_access_first_chance_handler(
    ScalarAccessFirstChanceHandler handler) {
  auto replacement =
      handler ? std::make_shared<const ScalarAccessFirstChanceHandler>(
                    std::move(handler))
              : nullptr;
  {
    auto lock = global_critical_region_.Acquire();
    scalar_access_first_chance_handler_.swap(replacement);
  }
  // A displaced callback's captures are also released outside the lock.
}

Function* Processor::RegisterScalarAccessFirstChance(GuestFunction* owner,
                                                     uint32_t guest_pc,
                                                     uint32_t raw_instruction) {
#if !XE_PLATFORM_WIN32 || !XE_ARCH_AMD64
  return nullptr;
#else
  if (!cvars::zero_page_scalar_first_chance || !cvars::protect_zero || !owner ||
      !builtin_module_ || !backend_ || (guest_pc & 3) ||
      !IsSupportedWord(raw_instruction) || guest_pc < owner->address() ||
      guest_pc > owner->end_address()) {
    return nullptr;
  }
  auto lock = global_critical_region_.Acquire();
  auto* module = owner->module();
  if (std::ranges::none_of(modules_, [module](const auto& candidate) {
        return candidate.get() == module;
      })) {
    return nullptr;
  }
  try {
    auto generation = scalar_access_module_generations_.find(module);
    if (generation == scalar_access_module_generations_.end()) {
      if (next_scalar_access_module_generation_ ==
          std::numeric_limits<uint64_t>::max()) {
        return nullptr;
      }
      generation = scalar_access_module_generations_
                       .emplace(module, next_scalar_access_module_generation_++)
                       .first;
    }
    const auto key =
        std::make_tuple(generation->second, guest_pc, raw_instruction);
    if (auto existing = scalar_access_first_chance_sites_.find(key);
        existing != scalar_access_first_chance_sites_.end()) {
      return existing->second->builtin;
    }
    if (scalar_access_first_chance_sites_.size() >= kMaxFirstChanceSites ||
        next_builtin_address_ < 0xFFFF0000u ||
        next_builtin_address_ > 0xFFFFFFF8u) {
      if (!scalar_access_site_limit_reported_) {
        scalar_access_site_limit_reported_ = true;
        XELOGW(
            "Scalar first-chance site/builtin limit reached; subsequent "
            "literal accesses retain native fault handling");
      }
      return nullptr;
    }
    auto site = std::make_unique<ScalarAccessFirstChanceSite>(
        ScalarAccessFirstChanceSite{module, generation->second, guest_pc,
                                    raw_instruction, nullptr});
    auto* registered = site.get();
    // Own the descriptor before publishing any builtin pointer to it. If a
    // subsequent allocation fails it remains owned, with a null builtin, and
    // future translations of the same site simply use native fault handling.
    scalar_access_first_chance_sites_.emplace(key, std::move(site));
    registered->builtin =
        DefineBuiltin("ScalarAccessFirstChance", ScalarAccessFirstChanceThunk,
                      this, registered);
    return registered->builtin;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
#endif
}

void Processor::ScalarAccessFirstChanceThunk(ppc::PPCContext* context,
                                             void* arg0, void* arg1) {
  static_cast<Processor*>(arg0)->OfferScalarAccessFirstChance(
      context, *static_cast<const ScalarAccessFirstChanceSite*>(arg1));
}

void Processor::OfferScalarAccessFirstChance(
    ppc::PPCContext* context, const ScalarAccessFirstChanceSite& site) {
  auto* thread = ThreadState::Get();
  if (!cvars::zero_page_scalar_first_chance || !cvars::protect_zero ||
      !thread || thread->processor() != this || thread->context() != context ||
      scalar_access_first_chance_active) {
    return;
  }
  const uint32_t address = LiteralAddress(site.raw_instruction);
  if (address > kZeroGuardEnd - 4) {
    return;
  }
  std::shared_ptr<const ScalarAccessFirstChanceHandler> handler;
  {
    auto lock = global_critical_region_.Acquire();
    const auto generation = scalar_access_module_generations_.find(site.module);
    if (generation == scalar_access_module_generations_.end() ||
        generation->second != site.module_generation ||
        LookupModule(site.guest_pc) != site.module ||
        !scalar_access_first_chance_handler_) {
      return;
    }
    auto* code_heap = memory_->LookupHeap(site.guest_pc);
    auto* data_heap = memory_->LookupHeap(address);
    if (!code_heap || !data_heap ||
        code_heap->QueryRangeAccess(site.guest_pc, site.guest_pc + 3) ==
            memory::PageAccess::kNoAccess ||
        data_heap->QueryRangeAccess(address, address + 3) !=
            memory::PageAccess::kNoAccess ||
        xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(site.guest_pc)) !=
            site.raw_instruction) {
      return;
    }
    handler = scalar_access_first_chance_handler_;
  }

  ppc::PPCContext saved;
  std::memcpy(&saved, context, sizeof(saved));
  const ScalarAccessFirstChance fault{
      site.guest_pc, site.raw_instruction, address,
      (site.raw_instruction >> 26) == 36, &saved};
  struct ActiveScope {
    bool& active;
    explicit ActiveScope(bool& value) : active(value) { active = true; }
    ~ActiveScope() { active = false; }
  } active(scalar_access_first_chance_active);
  // The retained immutable handler is invoked without any registry/memory lock.
  // It may enter guest code on this thread. No continuation result is accepted.
  (*handler)(thread, fault);
  // Only normal return is supported here. A terminal callback need not return;
  // this hook never changes the PC or skips the native memory operation.
  std::memcpy(context, &saved, sizeof(saved));
}

}  // namespace xe::cpu
