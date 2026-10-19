/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/xaudio2/xaudio2_audio_driver.h"

#include <atomic>
#include <chrono>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_queue_trace.h"
#include "xenia/apu/conversion.h"
#include "xenia/apu/xaudio2/xaudio2_api.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace xaudio2 {

namespace {

uint64_t AudioTraceNowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::atomic<uint64_t> next_trace_driver_id{1};
AudioQueueLogBudget trace_log_budget;
AudioQueueCheckpointBudget trace_checkpoint_budget;
// Observer-only rows, capped across all drivers. The ordinary audio path is
// unchanged when audio_queue_trace is false.
AudioQueueLogBudget transition_log_budget(512, 0);

template <typename AudioInterface>
void MaybeLogQueueCheckpoint(AudioQueueTrace& trace, AudioInterface* audio,
                             uint64_t sample_us) {
  AudioQueueTrace::Checkpoint checkpoint;
  if (!trace.TakeCheckpoint(sample_us, trace_checkpoint_budget, &checkpoint))
    return;
  // Each driver owns one XAudio2 engine. Query only on an already-due
  // checkpoint, after the original submit path, through the selected 2.7 or
  // 2.8+ interface. This is engine-wide, not a source-voice glitch count.
  api::XAUDIO2_PERFORMANCE_DATA performance{};
  const uint64_t engine_query_start_us = AudioTraceNowUs();
  audio->GetPerformanceData(&performance);
  const uint64_t engine_query_end_us = AudioTraceNowUs();
  // The collector mutex is released. These clocks are read only for a due
  // checkpoint, before synchronous logging under the caller's existing lock.
  const uint64_t log_start_us = AudioTraceNowUs();
  size_t transitions_emitted = 0;
  while (transitions_emitted < checkpoint.transition_count &&
         transition_log_budget.TakeRow())
    ++transitions_emitted;
  const auto& total = checkpoint.total;
  const int64_t min_queue =
      total.min_queue_before == AudioQueueTrace::kNoQueueSample
          ? -1
          : static_cast<int64_t>(total.min_queue_before);
  XELOGI(
      "APU XAudio2 queue live-checkpoint driver={} client={} ordinal={} "
      "sample_steady_us={} sample_bin={} log_start_steady_us={} "
      "cumulative_steady_us=[{},{}] attempts={} submitted={} "
      "submit_errors={} ratio_errors={} min_queued_before={} "
      "queue_zero={} queue_one={} max_success_gap_us={} "
      "max_success_gap_bin={} buffer_ends_sampled={} voice_errors_sampled={} "
      "engine_glitches_since_start={} engine_query_steady_us=[{},{}] "
      "transition_qpc_frequency={} transitions={} transition_rows={} "
      "transition_overwritten={} transition_log_omitted={}",
      trace.driver_id(), trace.client_index(), checkpoint.ordinal,
      checkpoint.sample_us, checkpoint.sample_bin, log_start_us,
      trace.start_us(), total.last_us, total.attempts, total.submitted,
      total.submit_errors, total.ratio_errors, min_queue, total.queue_zero,
      total.queue_one, total.max_success_gap_us,
      checkpoint.max_success_gap_bin, total.buffer_ends_sampled,
      total.voice_errors_sampled, performance.GlitchesSinceEngineStarted,
      engine_query_start_us, engine_query_end_us,
      checkpoint.qpc_frequency, checkpoint.transition_count,
      transitions_emitted, checkpoint.transitions_overwritten,
      checkpoint.transition_count - transitions_emitted);
  // Keep the newest events if the process-wide log budget is nearly exhausted.
  for (size_t i = checkpoint.transition_count - transitions_emitted;
       i < checkpoint.transition_count; ++i) {
    const auto& event = checkpoint.transitions[i];
    XELOGI(
        "APU XAudio2 queue transition driver={} client={} "
        "steady_us={} qpc_ticks={} became_empty={} depth=[{},{}] "
        "last_success_gap_us={} buffer_ends_seen={} pause_epoch={} "
        "submit_success={} ratio_error={}",
        trace.driver_id(), trace.client_index(), event.steady_us,
        event.qpc_ticks, event.became_empty, event.previous_depth,
        event.current_depth, event.last_success_gap_us,
        event.buffer_ends_seen, event.pause_epoch, event.submit_success,
        event.ratio_error);
  }
}

void LogQueueTraceBin(const AudioQueueTrace& trace, size_t index,
                      const AudioQueueTrace::Bin& bin) {
  const int64_t min_queue =
      bin.min_queue_before == AudioQueueTrace::kNoQueueSample
          ? -1
          : static_cast<int64_t>(bin.min_queue_before);
  XELOGI(
      "APU XAudio2 queue selected interval driver={} client={} bin={} "
      "steady_us=[{},{}] attempts={} submitted={} submit_errors={} ratio_errors={} "
      "min_queued_before={} queue_zero={} queue_one={} "
      "max_success_gap_us={} buffer_ends_sampled={} voice_errors_sampled={}",
      trace.driver_id(), trace.client_index(), index, bin.first_us,
      bin.last_us, bin.attempts, bin.submitted, bin.submit_errors,
      bin.ratio_errors, min_queue,
      bin.queue_zero, bin.queue_one, bin.max_success_gap_us,
      bin.buffer_ends_sampled, bin.voice_errors_sampled);
}

void FlushQueueTrace(const AudioQueueTrace& trace,
                     uint64_t teardown_start_us) {
  const auto total = trace.GetTotal();
  if (!total.attempts && !total.buffer_ends_sampled &&
      !total.voice_errors_sampled)
    return;
  // A selected row always needs its companion summary with teardown bounds.
  if (!trace_log_budget.TakeSummary()) return;
  const auto selection = trace.SelectWorstBins();
  const uint64_t flush_start_us = AudioTraceNowUs();
  size_t emitted = 0;
  for (size_t i = 0; i < selection.count; ++i) {
    if (!trace_log_budget.TakeRow()) break;
    const size_t bin_index = selection.indexes[i];
    LogQueueTraceBin(trace, bin_index, trace.GetBin(bin_index));
    ++emitted;
  }
  // This lower bound ends just before the final log call returns. Use the
  // whole teardown-adjacent range cautiously in cross-driver comparisons.
  const uint64_t before_summary_us = AudioTraceNowUs();
  const int64_t min_queue =
      total.min_queue_before == AudioQueueTrace::kNoQueueSample
          ? -1
          : static_cast<int64_t>(total.min_queue_before);
  XELOGI(
      "APU XAudio2 queue cumulative-through-shutdown driver={} client={} "
      "steady_us=[{},{}] teardown_steady_us=[{},{}] "
      "flush_steady_us=[{},{}] occupied_bins={} emitted_bins={} "
      "omitted_bins={} attempts={} submitted={} submit_errors={} "
      "ratio_errors={} min_queued_before={} queue_zero={} queue_one={} "
      "max_success_gap_us={} max_success_gap_bin={} "
      "buffer_ends_sampled={} voice_errors_sampled={}",
      trace.driver_id(), trace.client_index(), trace.start_us(),
      total.last_us, teardown_start_us, before_summary_us, flush_start_us,
      before_summary_us, selection.occupied, emitted,
      selection.occupied - emitted, total.attempts, total.submitted,
      total.submit_errors, total.ratio_errors, min_queue, total.queue_zero,
      total.queue_one, total.max_success_gap_us,
      selection.max_success_gap_bin,
      total.buffer_ends_sampled, total.voice_errors_sampled);
}

}  // namespace

class XAudio2AudioDriver::VoiceCallback : public api::IXAudio2VoiceCallback {
 public:
  VoiceCallback(xe::threading::Semaphore* semaphore, AudioQueueTrace* trace)
      : semaphore_(semaphore), trace_(trace) {}
  ~VoiceCallback() {}

  void OnStreamEnd() noexcept {}
  void OnVoiceProcessingPassEnd() noexcept {}
  void OnVoiceProcessingPassStart(uint32_t samples_required) noexcept {}
  void OnBufferEnd(void* context) noexcept {
    auto ret = semaphore_->Release(1, nullptr);
    assert_true(ret);
    if (trace_) trace_->BufferEnded();
  }
  void OnBufferStart(void* context) noexcept {}
  void OnLoopEnd(void* context) noexcept {}
  void OnVoiceError(void* context, HRESULT result) noexcept {
    if (trace_) trace_->VoiceErrored();
  }

 private:
  xe::threading::Semaphore* semaphore_ = nullptr;
  AudioQueueTrace* trace_ = nullptr;
};

XAudio2AudioDriver::XAudio2AudioDriver(xe::threading::Semaphore* semaphore,
                                       uint32_t frequency, uint32_t channels,
                                       bool need_format_conversion,
                                       int32_t client_index)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
  if (AudioQueueTraceEnabled()) {
    LARGE_INTEGER qpc_frequency{};
    QueryPerformanceFrequency(&qpc_frequency);
    queue_trace_ = std::make_unique<AudioQueueTrace>(
        next_trace_driver_id.fetch_add(1, std::memory_order_relaxed),
        client_index, AudioTraceNowUs(),
        AudioQueuePauseEpoch().load(std::memory_order_relaxed),
        static_cast<uint64_t>(qpc_frequency.QuadPart));
  }
}

XAudio2AudioDriver::~XAudio2AudioDriver() = default;

bool XAudio2AudioDriver::Initialize() {
  voice_callback_ = new VoiceCallback(semaphore_, queue_trace_.get());

  // Load the XAudio2 DLL dynamically. Needed both for 2.7 and for
  // differentiating between 2.8 and later versions. Windows 8.1 SDK references
  // XAudio2_8.dll in xaudio2.lib, which is available on Windows 8.1, however,
  // Windows 10 SDK references XAudio2_9.dll in it, which is only available in
  // Windows 10, and XAudio2_8.dll is linked through a different .lib -
  // xaudio2_8.lib, so easier not to link the .lib at all.
  xaudio2_module_ = static_cast<void*>(LoadLibraryW(L"XAudio2_8.dll"));
  if (xaudio2_module_) {
    xaudio2_create_ = reinterpret_cast<decltype(xaudio2_create_)>(
        GetProcAddress(static_cast<HMODULE>(xaudio2_module_), "XAudio2Create"));
    if (xaudio2_create_) {
      api_minor_version_ = 8;
    } else {
      XELOGE("XAudio2Create not found in XAudio2_8.dll");
      FreeLibrary(static_cast<HMODULE>(xaudio2_module_));
      xaudio2_module_ = nullptr;
    }
  }
  if (!xaudio2_module_) {
    XELOGE("Failed to load XAudio 2.8 library DLL");
    return false;
  }

  // We need to be able to accept frames from any non-STA thread - primarily
  // from any guest thread, so MTA needs to be used. The AudioDriver, however,
  // may be initialized from the UI thread, which has the STA concurrency model,
  // or from another thread regardless of its concurrency model. So, all
  // management of the objects needs to be performed in MTA. Launch the
  // lifecycle management thread, which will handle initialization and shutdown,
  // and also provide a scope for implicit MTA in threads that have never
  // initialized COM explicitly, which lasts until all threads that have
  // initialized MTA explicitly have uninitialized it - the thread that holds
  // the MTA scope needs to be running while other threads are able to submit
  // frames as they might have not initialized MTA explicitly.
  // https://devblogs.microsoft.com/oldnewthing/?p=4613
  // This is needed for both XAudio 2.7 (created via explicit CoCreateInstance)
  // and 2.8 (using XAudio2Create, but still requiring CoInitializeEx).
  // https://docs.microsoft.com/en-us/windows/win32/xaudio2/how-to--initialize-xaudio2
  assert_false(mta_thread_.joinable());
  mta_thread_initialization_attempt_completed_ = false;
  mta_thread_shutdown_requested_ = false;
  mta_thread_ = std::thread(&XAudio2AudioDriver::MTAThread, this);
  {
    std::unique_lock<std::mutex> mta_thread_initialization_completion_lock(
        mta_thread_initialization_completion_mutex_);
    while (true) {
      if (mta_thread_initialization_attempt_completed_) {
        break;
      }
      mta_thread_initialization_completion_cond_.wait(
          mta_thread_initialization_completion_lock);
    }
  }
  if (!mta_thread_initialization_completion_result_) {
    mta_thread_.join();
    return false;
  }
  return true;
}

template <typename Objects>
bool XAudio2AudioDriver::InitializeObjects(Objects& objects) {
  HRESULT hr;

  api::XAUDIO2_DEBUG_CONFIGURATION config;
  config.TraceMask = api::XE_XAUDIO2_LOG_ERRORS | api::XE_XAUDIO2_LOG_WARNINGS;
  config.BreakMask = 0;
  config.LogThreadID = false;
  config.LogTiming = true;
  config.LogFunctionName = true;
  config.LogFileline = true;
  objects.audio->SetDebugConfiguration(&config);

  hr = objects.audio->CreateMasteringVoice(&objects.mastering_voice);
  if (FAILED(hr)) {
    XELOGE("IXAudio2::CreateMasteringVoice failed with 0x{:08X}", hr);
    ShutdownObjects(objects);
    return false;
  }

  WAVEFORMATIEEEFLOATEX waveformat;

  waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  waveformat.Format.nChannels = frame_channels_;
  waveformat.Format.nSamplesPerSec = frame_frequency_;
  waveformat.Format.wBitsPerSample = 32;
  waveformat.Format.nBlockAlign =
      (waveformat.Format.nChannels * waveformat.Format.wBitsPerSample) / 8;
  waveformat.Format.nAvgBytesPerSec =
      waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
  waveformat.Format.cbSize =
      sizeof(WAVEFORMATIEEEFLOATEX) - sizeof(WAVEFORMATEX);

  waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  waveformat.Samples.wValidBitsPerSample = waveformat.Format.wBitsPerSample;
  static constexpr DWORD kChannelMasks[] = {
      0,
      0,
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
      0,
      0,
      0,
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_CENTER | SPEAKER_FRONT_RIGHT |
          SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT,
      0,
  };
  waveformat.dwChannelMask = kChannelMasks[waveformat.Format.nChannels];

  hr = objects.audio->CreateSourceVoice(
      &objects.pcm_voice, &waveformat.Format,
      0,  // api::XE_XAUDIO2_VOICE_NOSRC | api::XE_XAUDIO2_VOICE_NOPITCH,
      api::XE_XAUDIO2_MAX_FREQ_RATIO, voice_callback_);
  if (FAILED(hr)) {
    XELOGE("IXAudio2::CreateSourceVoice failed with 0x{:08X}", hr);
    ShutdownObjects(objects);
    return false;
  }

  hr = objects.pcm_voice->Start();
  if (FAILED(hr)) {
    XELOGE("IXAudio2SourceVoice::Start failed with 0x{:08X}", hr);
    ShutdownObjects(objects);
    return false;
  }

  if (cvars::mute) {
    objects.pcm_voice->SetVolume(0.0f);
  }

  return true;
}

void XAudio2AudioDriver::SubmitFrame(float* frame) {
  HRESULT hr;

  api::XAUDIO2_VOICE_STATE state;
  if (api_minor_version_ >= 8) {
    objects_.api_2_8.pcm_voice->GetState(&state,
                                         api::XE_XAUDIO2_VOICE_NOSAMPLESPLAYED);
  } else {
    objects_.api_2_7.pcm_voice->GetState(&state);
  }
  assert_true(state.BuffersQueued < frame_count_);
  const uint64_t trace_now_us = queue_trace_ ? AudioTraceNowUs() : 0;
  uint64_t trace_qpc_ticks = 0;
  if (queue_trace_) {
    LARGE_INTEGER qpc_now{};
    if (QueryPerformanceCounter(&qpc_now))
      trace_qpc_ticks = static_cast<uint64_t>(qpc_now.QuadPart);
  }

  auto output_frame = reinterpret_cast<float*>(frames_[current_frame_]);

  if (need_format_conversion_) {
    // Convert planar big endian samples into interleaved little endian.
    conversion::sequential_6_BE_to_interleaved_6_LE(output_frame, frame,
                                                    channel_samples_);
  } else {
    memcpy(output_frame, frame, frame_size_);
  }

  api::XAUDIO2_BUFFER buffer;
  buffer.Flags = 0;
  buffer.pAudioData = reinterpret_cast<BYTE*>(output_frame);
  buffer.AudioBytes = frame_size_;
  buffer.PlayBegin = 0;
  buffer.PlayLength = channel_samples_;
  buffer.LoopBegin = api::XE_XAUDIO2_NO_LOOP_REGION;
  buffer.LoopLength = 0;
  buffer.LoopCount = 0;
  buffer.pContext = 0;
  if (api_minor_version_ >= 8) {
    hr = objects_.api_2_8.pcm_voice->SubmitSourceBuffer(&buffer);
  } else {
    hr = objects_.api_2_7.pcm_voice->SubmitSourceBuffer(&buffer);
  }
  if (FAILED(hr)) {
    if (queue_trace_)
      queue_trace_->Submission(
          trace_now_us, state.BuffersQueued, false, false,
          AudioQueuePauseEpoch().load(std::memory_order_relaxed),
          trace_qpc_ticks);
    XELOGE("SubmitSourceBuffer failed with 0x{:08X}", hr);
    if (queue_trace_) {
      if (api_minor_version_ >= 8) {
        MaybeLogQueueCheckpoint(*queue_trace_, objects_.api_2_8.audio,
                                trace_now_us);
      } else {
        MaybeLogQueueCheckpoint(*queue_trace_, objects_.api_2_7.audio,
                                trace_now_us);
      }
    }
    return;
  }

  current_frame_ = (current_frame_ + 1) % frame_count_;

  // Update playback ratio to our time scalar.
  // This will keep audio in sync with the game clock.
  float frequency_ratio = static_cast<float>(xe::Clock::guest_time_scalar());
  HRESULT ratio_hr;
  if (api_minor_version_ >= 8) {
    ratio_hr = objects_.api_2_8.pcm_voice->SetFrequencyRatio(frequency_ratio);
  } else {
    ratio_hr = objects_.api_2_7.pcm_voice->SetFrequencyRatio(frequency_ratio);
  }
  if (queue_trace_) {
    queue_trace_->Submission(
        trace_now_us, state.BuffersQueued, true, FAILED(ratio_hr),
        AudioQueuePauseEpoch().load(std::memory_order_relaxed),
        trace_qpc_ticks);
    if (api_minor_version_ >= 8) {
      MaybeLogQueueCheckpoint(*queue_trace_, objects_.api_2_8.audio,
                              trace_now_us);
    } else {
      MaybeLogQueueCheckpoint(*queue_trace_, objects_.api_2_7.audio,
                              trace_now_us);
    }
  }
}

void XAudio2AudioDriver::Pause() {
  if (api_minor_version_ >= 8) {
    objects_.api_2_8.pcm_voice->Stop();
  } else {
    objects_.api_2_7.pcm_voice->Stop();
  }
  if (queue_trace_) queue_trace_->Pause();
}

void XAudio2AudioDriver::Resume() {
  if (api_minor_version_ >= 8) {
    objects_.api_2_8.pcm_voice->Start();
  } else {
    objects_.api_2_7.pcm_voice->Start();
  }
  if (queue_trace_) queue_trace_->Resume();
}

void XAudio2AudioDriver::SetVolume(float volume) {
  if (cvars::mute) {
    return;
  }

  if (api_minor_version_ >= 8) {
    objects_.api_2_8.pcm_voice->SetVolume(volume);
  } else {
    objects_.api_2_7.pcm_voice->SetVolume(volume);
  }
}

void XAudio2AudioDriver::Shutdown() {
  const uint64_t teardown_start_us = queue_trace_ ? AudioTraceNowUs() : 0;
  // XAudio2 lifecycle is managed by the MTA thread.
  if (mta_thread_.joinable()) {
    {
      std::unique_lock<std::mutex> mta_thread_shutdown_request_lock(
          mta_thread_shutdown_request_mutex_);
      mta_thread_shutdown_requested_ = true;
    }
    mta_thread_shutdown_request_cond_.notify_all();
    mta_thread_.join();
  }

  xaudio2_create_ = nullptr;
  if (xaudio2_module_) {
    FreeLibrary(static_cast<HMODULE>(xaudio2_module_));
    xaudio2_module_ = nullptr;
  }

  if (voice_callback_) {
    delete voice_callback_;
    voice_callback_ = nullptr;
  }
  if (queue_trace_) {
    queue_trace_->Finish(AudioTraceNowUs());
    FlushQueueTrace(*queue_trace_, teardown_start_us);
    queue_trace_.reset();
  }
}

template <typename Objects>
void XAudio2AudioDriver::ShutdownObjects(Objects& objects) {
  // Stop the engine first to ensure no callbacks are in-flight before
  // destroying voices and the callback object.
  if (objects.audio) {
    objects.audio->StopEngine();
  }

  if (objects.pcm_voice) {
    objects.pcm_voice->DestroyVoice();
    objects.pcm_voice = nullptr;
  }

  if (objects.mastering_voice) {
    objects.mastering_voice->DestroyVoice();
    objects.mastering_voice = nullptr;
  }

  if (objects.audio) {
    objects.audio->Release();
    objects.audio = nullptr;
  }
}

void XAudio2AudioDriver::MTAThread() {
  xe::threading::set_name("XAudio2 MTA");

  assert_false(mta_thread_initialization_attempt_completed_);

  bool initialized = false;

  // Initializing MTA COM in this thread, as well making other (guest) threads
  // that don't explicitly call CoInitializeEx implicitly MTA for the period of
  // time when they can interact with XAudio through the XAudio2AudioDriver,
  // until the CoUninitialize (to be more precise, the CoUninitialize for the
  // last remaining MTA thread, but we need implicit MTA for the audio here).
  // https://devblogs.microsoft.com/oldnewthing/?p=4613
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(hr);
  if (!com_initialized) {
    XELOGE("XAudio2 MTA thread CoInitializeEx failed with 0x{:08X}", hr);
  } else {
    if (api_minor_version_ >= 8) {
      hr = xaudio2_create_(&objects_.api_2_8.audio, 0, kProcessor);
      if (FAILED(hr)) {
        XELOGE("XAudio2Create failed with 0x{:08X}", hr);
      } else {
        initialized = InitializeObjects(objects_.api_2_8);
      }
    } else {
      hr = CoCreateInstance(__uuidof(api::XAudio2_7), nullptr,
                            CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&objects_.api_2_7.audio));
      if (FAILED(hr)) {
        XELOGE("CoCreateInstance for XAudio2 failed with 0x{:08X}", hr);
      } else {
        hr = objects_.api_2_7.audio->Initialize(0, kProcessor);
        if (FAILED(hr)) {
          XELOGE("IXAudio2::Initialize failed with 0x{:08X}", hr);
        } else {
          initialized = InitializeObjects(objects_.api_2_7);
        }
      }
    }
  }

  // Notify the threads waiting for the initialization of the result.
  mta_thread_initialization_completion_result_ = initialized;
  {
    std::unique_lock<std::mutex> mta_thread_initialization_completion_lock(
        mta_thread_initialization_completion_mutex_);
    mta_thread_initialization_attempt_completed_ = true;
  }
  mta_thread_initialization_completion_cond_.notify_all();

  if (initialized) {
    // Initialized successfully, await a shutdown request while keeping an
    // implicit COM MTA scope.
    {
      std::unique_lock<std::mutex> mta_thread_shutdown_request_lock(
          mta_thread_shutdown_request_mutex_);
      while (true) {
        if (mta_thread_shutdown_requested_) {
          break;
        }
        mta_thread_shutdown_request_cond_.wait(
            mta_thread_shutdown_request_lock);
      }
    }

    if (api_minor_version_ >= 8) {
      ShutdownObjects(objects_.api_2_8);
    } else {
      ShutdownObjects(objects_.api_2_7);
    }
  }

  if (com_initialized) {
    CoUninitialize();
  }
}

}  // namespace xaudio2
}  // namespace apu
}  // namespace xe
