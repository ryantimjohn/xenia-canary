// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork diagnostic; inherited SDK-derived ancestry remains fork-only.
#ifndef XENIA_KERNEL_XAM_INPUT_OBSERVE_H_
#define XENIA_KERNEL_XAM_INPUT_OBSERVE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "xenia/hid/input.h"
#include "xenia/xbox.h"

namespace xe::kernel::xam::input_observe {

enum class Api : uint32_t { kState, kKeystroke, kKeystrokeEx };
enum class Route : uint32_t { kInvalidOutput, kInvalidUser, kUI, kDriver };

struct Completion {
  Route route = Route::kInvalidOutput;
  uint32_t result = X_ERROR_BAD_ARGUMENTS;
  uint32_t user = 0, flags = 0, driver_calls = 0;
  bool user_valid = false, ui_checked = false, ui_active = false;
  bool output_initialized = false, output_valid = false;
  bool a = false;
  uint32_t packet = 0;
  uint16_t virtual_key = 0, edge_flags = 0;

  // Packet churn alone must not spend the bounded row budget.
  bool Same(const Completion& b) const {
    return route == b.route && result == b.result && user == b.user &&
           flags == b.flags && driver_calls == b.driver_calls &&
           user_valid == b.user_valid && ui_checked == b.ui_checked &&
           ui_active == b.ui_active &&
           output_initialized == b.output_initialized &&
           output_valid == b.output_valid && a == b.a &&
           virtual_key == b.virtual_key && edge_flags == b.edge_flags;
  }
};

// These are the production dispatch paths. Dependencies are callbacks so tests
// can assert that early gates never acquire the input lock or read user_ptr.
// WithDriver owns the existing input lock for the entire operation (including
// the any-user loop), and releases it before observation/logging.
template <typename UI, typename WithDriver>
uint32_t GetState(uint32_t user, uint32_t flags, hid::X_INPUT_STATE* output,
                  UI&& is_ui_active, WithDriver&& with_driver, Completion& c) {
  c.user = user;
  c.user_valid = true;
  c.flags = flags;
  if (output) {
    std::memset(output, 0, sizeof(*output));
    c.output_initialized = true;
  }
  if (user >= XUserMaxUserCount) {
    c.route = Route::kInvalidUser;
    return c.result = X_ERROR_DEVICE_NOT_CONNECTED;
  }
  c.ui_checked = true;
  c.ui_active = is_ui_active();
  if (c.ui_active) {
    c.route = Route::kUI;
    return c.result = X_ERROR_SUCCESS;
  }
  c.route = Route::kDriver;
  return c.result = with_driver([&](auto& driver) {
           ++c.driver_calls;
           return driver.GetState(
               user, !flags ? hid::X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD : flags,
               output);
         });
}

template <typename UI, typename WithDriver>
uint32_t GetKeystroke(be<uint32_t>* user_ptr, uint32_t flags,
                      hid::X_INPUT_KEYSTROKE* output, UI&& is_ui_active,
                      WithDriver&& with_driver, Completion& c) {
  c.flags = flags;
  if (!output) {
    return c.result = X_ERROR_BAD_ARGUMENTS;
  }
  std::memset(output, 0, sizeof(*output));
  c.output_initialized = true;
  c.ui_checked = true;
  c.ui_active = is_ui_active();
  if (c.ui_active) {
    c.route = Route::kUI;
    return c.result = X_ERROR_SUCCESS;
  }
  // Preserve the original dereference location: UI gate must not read this.
  uint32_t user = *user_ptr;
  c.user = user;
  c.user_valid = true;
  c.route = Route::kDriver;
  return c.result = with_driver([&](auto& driver) -> uint32_t {
           if ((user & XUserIndexAny) == XUserIndexAny) {
             user = 0;
           }
           if (flags & hid::X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER) {
             auto result = X_ERROR_DEVICE_NOT_CONNECTED;
             for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
               ++c.driver_calls;
               auto result = driver.GetKeystroke(i, flags, output);
               if (result == X_ERROR_SUCCESS) {
                 *user_ptr = output->user_index;
                 return result;
               }
             }
             // Keep existing any-user return behavior, including its outer
             // result.
             return result;
           }
           ++c.driver_calls;
           auto result = driver.GetKeystroke(user, flags, output);
           if (XSUCCEEDED(result)) {
             *user_ptr = output->user_index;
           }
           return result;
         });
}

inline void Snapshot(Completion& c, const hid::X_INPUT_STATE* output) {
  c.output_valid =
      output && c.output_initialized && c.result == X_ERROR_SUCCESS;
  if (c.output_valid) {
    c.packet = output->packet_number;
    c.a = (output->gamepad.buttons & 0x1000) != 0;
  }
}
inline void Snapshot(Completion& c, const hid::X_INPUT_KEYSTROKE* output) {
  c.output_valid =
      output && c.output_initialized && c.result == X_ERROR_SUCCESS;
  if (c.output_valid) {
    c.virtual_key = output->virtual_key;
    c.edge_flags = output->flags;
  }
}

class Collector {
 public:
  struct Entry {
    bool enabled = false, anchor = false;
    Api api = Api::kState;
    uint32_t thread = 0;
    uint64_t lr = 0, epoch_ms = 0, anchor_ms = 0, sequence = 0;
  };
  struct Counts {
    uint64_t entries = 0, completions = 0, gates = 0, driver_calls = 0;
  };
  struct Event {
    Entry entry;
    Completion completion;
    bool row = false, cap_notice = false, summary = false;
    uint32_t row_number = 0;
    uint64_t through_epoch_ms = 0;
    std::array<Counts, 3> counts{};
  };

  explicit Collector(bool enabled) : enabled_(enabled) {}
  bool enabled() const { return enabled_; }
  Entry Begin(Api api, uint32_t thread, uint64_t lr, uint64_t epoch_ms) {
    if (!enabled_) {
      return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool first = !started_;
    if (first) {
      started_ = true;
      anchor_ms_ = epoch_ms;
    }
    auto& counts = counts_[Index(api)];
    Increment(counts.entries);
    Increment(sequence_);
    return {true, first, api, thread, lr, epoch_ms, anchor_ms_, sequence_};
  }
  Event End(const Entry& entry, const Completion& completion,
            uint64_t epoch_ms) {
    if (!entry.enabled) {
      return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& counts = counts_[Index(entry.api)];
    Increment(counts.completions);
    if (completion.route == Route::kUI) {
      Increment(counts.gates);
    }
    for (uint32_t i = 0; i < completion.driver_calls; ++i) {
      Increment(counts.driver_calls);
    }
    Event event;
    event.entry = entry;
    event.completion = completion;
    event.through_epoch_ms = epoch_ms;
    // Window is selected by shim ENTRY time, not completion time or replay
    // time. Concurrent entrants can sample their timestamps in the opposite
    // order from acquiring this mutex. An earlier sample must not underflow.
    const uint64_t elapsed =
        entry.epoch_ms >= anchor_ms_ ? entry.epoch_ms - anchor_ms_ : 0;
    if (elapsed >= 180000 && elapsed < 240000 && rows_ < 64) {
      auto& previous = previous_[Index(entry.api)];
      if (!previous.valid || previous.thread != entry.thread ||
          previous.lr != entry.lr || !previous.completion.Same(completion)) {
        previous = {true, entry.thread, entry.lr, completion};
        event.row = true;
        event.row_number = ++rows_;
        event.cap_notice = rows_ == 64;
      }
    }
    if (elapsed >= 240000 && !summary_emitted_) {
      summary_emitted_ = true;
      event.summary = true;
    }
    event.counts = counts_;
    return event;
  }

 private:
  static size_t Index(Api api) { return static_cast<size_t>(api); }
  static void Increment(uint64_t& value) {
    if (value != UINT64_MAX) {
      ++value;
    }
  }
  struct Previous {
    bool valid = false;
    uint32_t thread = 0;
    uint64_t lr = 0;
    Completion completion;
  };
  const bool enabled_;
  std::mutex mutex_;
  bool started_ = false, summary_emitted_ = false;
  uint64_t anchor_ms_ = 0, sequence_ = 0;
  uint32_t rows_ = 0;
  std::array<Counts, 3> counts_{};
  std::array<Previous, 3> previous_{};
};

}  // namespace xe::kernel::xam::input_observe
#endif  // XENIA_KERNEL_XAM_INPUT_OBSERVE_H_
