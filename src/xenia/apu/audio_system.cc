/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include <atomic>
#include <limits>
#include <memory>
#include <optional>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_pump_observer.h"
#include "xenia/apu/audio_queue_trace.h"
#include "xenia/apu/nop_audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace xe {
namespace apu {

namespace {

bool PumpObserverEnabled() {
  static const bool enabled = cvars::audio_pump_observer;
  return enabled;
}

uint64_t PumpQpc() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER value{};
  return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
#else
  return 0;
#endif
}

uint64_t PumpQpf() {
#if XE_PLATFORM_WIN32
  LARGE_INTEGER value{};
  return QueryPerformanceFrequency(&value) ? uint64_t(value.QuadPart) : 0;
#else
  return 0;
#endif
}

pump_observer::Stamp PumpStamp() {
  const uint64_t before = PumpQpc();
  const uint64_t steady_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  const uint64_t after = PumpQpc();
  const bool valid = before && after && after >= before;
  return {steady_us, valid ? after : 0, valid ? after - before : 0, true};
}

enum class PumpLifecycleKind { kRegister, kUnregister, kInvalidate };

pump_observer::LifecycleBudget& PumpLifecycleBudget() {
  static pump_observer::LifecycleBudget budget;
  return budget;
}

pump_observer::WindowBudget& PumpWindowBudget() {
  static pump_observer::WindowBudget budget;
  return budget;
}

std::atomic<uint64_t>& PumpLifecycleLogCost() {
  static std::atomic<uint64_t> cost{0};
  return cost;
}

void LogPumpLifecycle(const AudioSystem* owner, PumpLifecycleKind kind,
                      size_t client_index, uint32_t callback,
                      uint32_t wrapped_arg) {
  const bool registration = kind == PumpLifecycleKind::kRegister;
  const auto claim = PumpLifecycleBudget().Claim(registration);
  if (!claim.emit) {
    return;
  }
  const uint64_t at = PumpQpc();
  if (claim.cutoff) {
    XELOGI(
        "APUMP001 lifecycle_cap owner={:p} at_qpc={} "
        "identity_after_cap=unknown",
        static_cast<const void*>(owner), at);
  } else {
    const char* name = kind == PumpLifecycleKind::kRegister     ? "register"
                       : kind == PumpLifecycleKind::kUnregister ? "unregister"
                                                                : "invalidate";
    XELOGI(
        "APUMP001 lifecycle owner={:p} kind={} serial={} slot={} "
        "callback={} wrapped_arg={} at_qpc={}",
        static_cast<const void*>(owner), name, claim.serial, client_index,
        callback, wrapped_arg, at);
  }
  const uint64_t after = PumpQpc();
  if (at && after >= at) {
    PumpLifecycleLogCost().fetch_add(after - at);
  }
}

void LogPumpDetail(const AudioSystem* owner, uint32_t window, const char* role,
                   const pump_observer::Detail& detail) {
  if (!detail.present) {
    return;
  }
  const auto& a = detail.attempt;
  XELOGI(
      "APUMP001 detail owner={:p} window={} role={} slot={} callback={} "
      "wrapped_arg={} outcome={} deadline_us={} selected_now_us={} "
      "next_us={} selected_stamp_us={} selected_qpc={} wake_us={} "
      "wake_qpc={} callback_begin_us={} callback_begin_qpc={} "
      "callback_end_us={} callback_end_qpc={} logging_adjacent={}",
      static_cast<const void*>(owner), window, role, a.client_index, a.callback,
      a.wrapped_callback_arg, uint32_t(a.outcome), a.deadline_us,
      a.selected_now_us, a.scheduled_next_us, a.selected.steady_us,
      a.selected.qpc, a.wake.steady_us, a.wake.qpc, a.callback_begin.steady_us,
      a.callback_begin.qpc, a.callback_end.steady_us, a.callback_end.qpc,
      a.logging_adjacent);
}

uint64_t LogPumpReport(const AudioSystem* owner, uint32_t window, uint64_t qpf,
                       const pump_observer::Report& report,
                       uint64_t previous_log_cost, bool previous_log_cost_valid,
                       uint64_t* log_end_qpc) {
  const uint64_t begin = PumpQpc();
  const uint64_t lifecycle_rows = PumpLifecycleBudget().observed();
  XELOGI(
      "APUMP001 window owner={:p} ordinal={} qpf={} start_us={} end_us={} "
      "first_qpc={} last_qpc={} emitted_qpc={} selected={} "
      "no_callback={} interrupted={} token_timeout={} token_other={} "
      "driver_missing={} executed={} skipped_empty_windows={} "
      "explicit_qpc_queries={} stamp_bracket_ticks={} "
      "invalid_qpc_stamps={} "
      "logging_adjacent={} lifecycle_rows={} identity_unknown={} "
      "lifecycle_log_enqueue_span_ticks={} "
      "previous_window_log_enqueue_span_ticks={} "
      "previous_log_cost_valid={} cap_reached={} final_partial=unknown",
      static_cast<const void*>(owner), window, qpf, report.window_start_us,
      report.window_end_us, report.first_selected_qpc, report.last_selected_qpc,
      begin, report.selected,
      report.outcomes[size_t(pump_observer::Outcome::kNoCallback)],
      report.outcomes[size_t(pump_observer::Outcome::kInterrupted)],
      report.outcomes[size_t(pump_observer::Outcome::kTokenTimeout)],
      report.outcomes[size_t(pump_observer::Outcome::kTokenOther)],
      report.outcomes[size_t(pump_observer::Outcome::kDriverMissing)],
      report.outcomes[size_t(pump_observer::Outcome::kExecuted)],
      report.skipped_empty_windows, report.explicit_qpc_queries,
      report.stamp_bracket_ticks, report.invalid_qpc_stamps,
      report.logging_adjacent, lifecycle_rows,
      PumpLifecycleBudget().identity_unknown(), PumpLifecycleLogCost().load(),
      previous_log_cost, previous_log_cost_valid, report.cap_reached);
  LogPumpDetail(owner, window, "worst_selected", report.worst_selected);
  LogPumpDetail(owner, window, "worst_wake", report.worst_wake);
  LogPumpDetail(owner, window, "worst_callback", report.worst_callback);
  LogPumpDetail(owner, window, "worst_token_timeout",
                report.worst_token_timeout);
  const uint64_t end = PumpQpc();
  *log_end_qpc = begin && end >= begin ? end : 0;
  return begin && end >= begin ? end - begin : 0;
}

}  // namespace

AudioSystem::AudioSystem(cpu::Processor* processor)
    : memory_(processor->memory()),
      processor_(processor),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] =
        xe::threading::Semaphore::Create(0, kMaximumQueuedFrames);
  }
  pending_work_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(pending_work_event_);

  xma_decoder_ = std::make_unique<xe::apu::XmaDecoder>(processor_);

  resume_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state->GetSystemProcess()));
  // As we run audio callbacks the debugger must be able to suspend us.
  worker_thread_->set_can_debugger_suspend(true);
  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();
  // Set high priority for this thread for better pacing.
  worker_thread_->SetPriority(24);
  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Process-retained log windows, owned only by this worker. The disabled
  // path creates no collector and makes no extra clock or logging calls.
  std::unique_ptr<pump_observer::Collector> pump_observer;
  uint64_t pump_qpf = 0;
  uint64_t previous_log_cost = 0;
  uint64_t last_log_end_qpc = 0;
  bool previous_log_cost_valid = false;
  if (PumpObserverEnabled() && !PumpWindowBudget().exhausted() &&
      PumpWindowBudget().ClaimSession()) {
    pump_observer = std::make_unique<pump_observer::Collector>();
    pump_qpf = PumpQpf();
    XELOGI(
        "APUMP001 start owner={:p} qpf={} start_qpc={} "
        "window_us={} max_windows={} max_sessions={} "
        "max_details_per_window=4 "
        "lifecycle_cap={} paired_audio_queue_trace={} "
        "pre_first_selection_and_final_partial=unqualified",
        static_cast<const void*>(this), pump_qpf, PumpQpc(),
        pump_observer::kWindowUs, pump_observer::kMaxWindows,
        pump_observer::kMaxSessions, pump_observer::kMaxLifecycleRows,
        AudioQueueTraceEnabled());
  }
  auto record_pump = [&](const pump_observer::Attempt& attempt) {
    if (!pump_observer) {
      return;
    }
    pump_observer::Report completed;
    if (pump_observer->Observe(attempt, &completed)) {
      const auto claim = PumpWindowBudget().Claim();
      if (!claim.emit) {
        pump_observer.reset();
        return;
      }
      completed.cap_reached |= claim.final;
      previous_log_cost = LogPumpReport(
          this, pump_observer->reports_emitted(), pump_qpf, completed,
          previous_log_cost, previous_log_cost_valid, &last_log_end_qpc);
      previous_log_cost_valid = last_log_end_qpc != 0;
      if (completed.cap_reached) {
        pump_observer.reset();
      }
    }
  };

  // The host mixer releases a client's semaphore on its own coarse cadence,
  // but Xenos audio subsystem operates at 5.333ms interval (see
  // xaudio2_audio_driver.cc) Interval scales inversely with guest_time_scalar.
  // We therefore pace pumps to each client's next_pump_us deadline and use
  // the semaphore only as back-pressure: a frame is submitted only if
  // a host output slot is free, otherwise it is dropped (the host queue
  // is full, so it is already well buffered).
  while (worker_running_) {
    if (pump_observer && PumpWindowBudget().exhausted()) {
      pump_observer.reset();
    }
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    size_t client_index = kMaximumClientCount;
    uint64_t earliest_pump_us = std::numeric_limits<uint64_t>::max();
    uint64_t scheduled_next_us = 0;
    uint32_t client_callback = 0;
    uint32_t client_callback_arg = 0;
    pump_observer::Stamp selected_stamp;
    {
      auto global_lock = global_critical_region_.Acquire();

      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (!clients_[i].in_use ||
            clients_[i].next_pump_us >= earliest_pump_us) {
          continue;
        }
        earliest_pump_us = clients_[i].next_pump_us;
        client_index = i;
      }

      if (client_index != kMaximumClientCount) {
        client_callback = clients_[client_index].callback;
        client_callback_arg = clients_[client_index].wrapped_callback_arg;

        const double scalar = xe::Clock::guest_time_scalar();
        const uint64_t min_us =
            scalar > 0.0 ? static_cast<uint64_t>(kAudioPumpInterval / scalar)
                         : kAudioPumpInterval;
        scheduled_next_us =
            (earliest_pump_us > now ? earliest_pump_us : now) + min_us;
        clients_[client_index].next_pump_us = scheduled_next_us;
        // Stamp while the existing global lock still establishes registration
        // order; the enabled query cost is reported in the window.
        if (pump_observer) {
          selected_stamp = PumpStamp();
        }
      }
    }

    // No clients yet: park until one registers or we're told to stop.
    if (client_index == kMaximumClientCount) {
      xe::threading::Wait(pending_work_event_.get(), true);
      if (paused_) {
        pause_fence_.Signal();
        xe::threading::Wait(resume_event_.get(), false);
      }
      continue;
    }

    std::optional<pump_observer::Attempt> pump_attempt;
    if (pump_observer) {
      pump_attempt.emplace();
      pump_attempt->selected_now_us = now;
      pump_attempt->deadline_us = earliest_pump_us;
      pump_attempt->scheduled_next_us = scheduled_next_us;
      pump_attempt->client_index = uint32_t(client_index);
      pump_attempt->callback = client_callback;
      pump_attempt->wrapped_callback_arg = client_callback_arg;
      pump_attempt->selected = selected_stamp;
      pump_attempt->logging_adjacent =
          last_log_end_qpc && pump_qpf && pump_attempt->selected.qpc &&
          pump_attempt->selected.qpc >= last_log_end_qpc &&
          pump_attempt->selected.qpc - last_log_end_qpc <= pump_qpf / 100;
    }

    // Pace to kAudioIntervalSlack ahead of the deadline.
    const uint64_t wake_target_us = earliest_pump_us > kAudioIntervalSlack
                                        ? earliest_pump_us - kAudioIntervalSlack
                                        : 0;
    if (wake_target_us > now) {
      const std::chrono::milliseconds timeout((wake_target_us - now) / 1000);
      auto result =
          xe::threading::Wait(pending_work_event_.get(), true, timeout);
      if (result == xe::threading::WaitResult::kSuccess) {
        if (pump_observer) {
          pump_attempt->wake = PumpStamp();
          pump_attempt->outcome = pump_observer::Outcome::kInterrupted;
        }
        if (paused_) {
          pause_fence_.Signal();
          xe::threading::Wait(resume_event_.get(), false);
        }
        if (pump_attempt) {
          record_pump(*pump_attempt);
        }
        continue;
      }

      const uint64_t now_precise = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (wake_target_us > now_precise) {
        xe::threading::NanoSleepPrecise((wake_target_us - now_precise) * 1000);
      }
    }

    // Submit only if the host has a free output slot;
    if (pump_observer) {
      pump_attempt->wake = PumpStamp();
    }
    if (client_callback) {
      const auto token_result =
          xe::threading::Wait(client_semaphores_[client_index].get(), false,
                              std::chrono::milliseconds(0));
      if (token_result == xe::threading::WaitResult::kSuccess) {
        std::lock_guard<std::mutex> guard(clients_[client_index].lock);
        if (clients_[client_index].driver == nullptr) {
          if (pump_observer) {
            pump_attempt->outcome = pump_observer::Outcome::kDriverMissing;
          } else {
            // Preserve the original disabled-path early exit.
            continue;
          }
        } else {
          if (pump_observer) {
            pump_attempt->callback_begin = PumpStamp();
          }
          SCOPE_profile_cpu_i("apu", "xe::apu::AudioSystem->client_callback");
          uint64_t args[] = {client_callback_arg};
          processor_->Execute(worker_thread_->thread_state(), client_callback,
                              args, xe::countof(args));
          if (pump_observer) {
            pump_attempt->callback_end = PumpStamp();
            pump_attempt->outcome = pump_observer::Outcome::kExecuted;
          }
        }
      } else if (pump_observer) {
        pump_attempt->outcome =
            token_result == xe::threading::WaitResult::kTimeout
                ? pump_observer::Outcome::kTokenTimeout
                : pump_observer::Outcome::kTokenOther;
      }
    }
    if (pump_attempt) {
      record_pump(*pump_attempt);
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (int i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  worker_running_ = false;
  pending_work_event_->Set();
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }

  // Unregister all active clients to shut down their audio drivers before
  // the semaphores are destroyed with this AudioSystem.
  {
    auto global_lock = global_critical_region_.Acquire();
    // Teardown does not call UnregisterClient for each slot. End all
    // observer-only slot identities before the first driver is destroyed.
    if (PumpObserverEnabled()) {
      LogPumpLifecycle(this, PumpLifecycleKind::kInvalidate,
                       kMaximumClientCount, 0, 0);
    }
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      if (clients_[i].in_use) {
        DestroyDriver(clients_[i].driver);
        clients_[i].driver = nullptr;
        if (clients_[i].wrapped_callback_arg) {
          memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
        }
        clients_[i].callback = 0;
        clients_[i].callback_arg = 0;
        clients_[i].wrapped_callback_arg = 0;
        clients_[i].in_use = false;
      }
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(kMaximumQueuedFrames, nullptr);
  assert_true(ret);

  AudioDriver* driver = nullptr;
  auto result = CreateClientDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    XELOGE("AudioSystem::RegisterClient: CreateDriver failed for index={}",
           index);
    return result;
  }
  assert_not_null(driver);
  XELOGI(
      "AudioSystem::RegisterClient: driver created for index={}, driver={:p}",
      index, (void*)driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index].driver = driver;
  clients_[index].callback = callback;
  clients_[index].callback_arg = callback_arg;
  clients_[index].wrapped_callback_arg = ptr;
  clients_[index].in_use = true;

  if (PumpObserverEnabled()) {
    LogPumpLifecycle(this, PumpLifecycleKind::kRegister, size_t(index),
                     callback, ptr);
  }

  // Wake the worker so it re-scans and starts pacing this client immediately.
  pending_work_event_->Set();

  XELOGI("AudioSystem::RegisterClient: client {} registered successfully",
         index);

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, float* samples) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  if (index >= kMaximumClientCount || !clients_[index].in_use ||
      !clients_[index].driver) {
    XELOGW(
        "SubmitFrame called for invalid/unregistered client index {} "
        "(in_use={}, driver={:p})",
        index, index < kMaximumClientCount ? clients_[index].in_use : false,
        index < kMaximumClientCount ? (void*)clients_[index].driver : nullptr);

    // Submit silence instead of dropping the frame to maintain the callback
    // chain.  If we don't submit anything, the audio driver's OnBufferEnd
    // callback will never fire, causing the semaphore to leak.
    if (index < kMaximumClientCount && clients_[index].driver) {
      static float silence[apu::AudioDriver::kFrameSamplesMax] = {0};
      (clients_[index].driver)->SubmitFrame(silence);
    }
    return;
  }
  (clients_[index].driver)->SubmitFrame(samples);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);

  std::lock_guard<std::mutex> guard(clients_[index].lock);

  DestroyDriver(clients_[index].driver);
  clients_[index].driver = nullptr;
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index].callback = 0;
  clients_[index].callback_arg = 0;
  clients_[index].wrapped_callback_arg = 0;
  clients_[index].in_use = false;

  if (PumpObserverEnabled()) {
    LogPumpLifecycle(this, PumpLifecycleKind::kUnregister, index, 0, 0);
  }

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  xe::threading::WaitResult wait_result;
  do {
    wait_result = xe::threading::Wait(client_semaphore, false,
                                      std::chrono::milliseconds(0));
  } while (wait_result == xe::threading::WaitResult::kSuccess);
  assert_true(wait_result == xe::threading::WaitResult::kTimeout);
}

bool AudioSystem::Save(ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (int i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    XELOGE("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  // Restore may recreate a driver and slot without RegisterClient. Its
  // mapping must not inherit a prior registration serial.
  if (PumpObserverEnabled()) {
    LogPumpLifecycle(this, PumpLifecycleKind::kInvalidate, kMaximumClientCount,
                     0, 0);
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.next_pump_us = 0;
    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(kMaximumQueuedFrames, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateClientDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      XELOGE(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  pending_work_event_->Set();
  pause_fence_.Wait();
  if (AudioQueueTraceEnabled()) {
    AudioQueuePauseEpoch().fetch_add(1, std::memory_order_relaxed);
  }

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  if (AudioQueueTraceEnabled()) {
    AudioQueuePauseEpoch().fetch_add(1, std::memory_order_relaxed);
  }

  resume_event_->Set();

  xma_decoder_->Resume();
}

X_STATUS AudioSystem::CreateClientDriver(size_t index,
                                         xe::threading::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  *out_driver = nullptr;
  X_STATUS result = CreateDriver(index, semaphore, out_driver);
  if (result == X_STATUS_SUCCESS && *out_driver) {
    return result;
  }

  if (*out_driver) {
    DestroyDriver(*out_driver);
  }
  // The audio worker paces registered clients; release their slots immediately.
  *out_driver = new nop::NopAudioDriver(semaphore);
  XELOGW("APU: {} failed to create a driver for client {}; continuing silently",
         name(), index);
  return X_STATUS_SUCCESS;
}

void AudioSystem::DestroyDriver(AudioDriver* driver) {
  if (driver) {
    driver->Shutdown();
    delete driver;
  }
}

}  // namespace apu
}  // namespace xe
