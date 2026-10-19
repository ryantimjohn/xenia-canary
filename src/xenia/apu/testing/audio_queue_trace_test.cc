/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/apu/audio_queue_trace.h"

#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::apu::test {

TEST_CASE("Audio queue trace excludes startup priming and exposes a later gap",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(7, 2, 1000000, 0);
  trace.Submission(1000000, 0, true, false, 0);
  trace.Submission(1005333, 2, true, false, 0);
  trace.Submission(1030000, 0, true, false, 0);
  const auto total = trace.GetTotal();
  CHECK(total.attempts == 3);
  CHECK(total.submitted == 3);
  CHECK(total.queue_zero == 1);
  CHECK(total.min_queue_before == 0);
  CHECK(total.max_success_gap_us == 24667);
  CHECK(trace.driver_id() == 7);
  CHECK(trace.client_index() == 2);
}

TEST_CASE("Audio queue trace separates errors from successful submit gaps",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(8, -1, 1000000, 0);
  trace.Submission(1000000, 0, true, false, 0);
  trace.Submission(1005333, 1, false, false, 0);
  trace.Submission(1010666, 2, true, true, 0);
  const auto total = trace.GetTotal();
  CHECK(total.attempts == 3);
  CHECK(total.submitted == 2);
  CHECK(total.submit_errors == 1);
  CHECK(total.ratio_errors == 1);
  CHECK(total.queue_one == 1);
  CHECK(total.max_success_gap_us == 10666);
}

TEST_CASE("Audio queue trace resets priming after explicit and system pause",
          "[audio-queue-trace]") {
  AudioQueueTrace media(9, -1, 1000000, 0);
  media.Submission(1000000, 0, true, false, 0);
  media.Pause();
  media.Resume();
  media.Submission(5000000, 0, true, false, 0);
  CHECK(media.GetTotal().queue_zero == 0);
  CHECK(media.GetTotal().max_success_gap_us == 0);
  AudioQueueTrace client(10, 0, 1000000, 0);
  client.Submission(1000000, 0, true, false, 0);
  client.Submission(5000000, 0, true, false, 2);
  CHECK(client.GetTotal().queue_zero == 0);
  CHECK(client.GetTotal().max_success_gap_us == 0);
  client.Submission(5005333, 0, true, false, 2);
  CHECK(client.GetTotal().queue_zero == 1);
}

TEST_CASE("Audio queue trace samples callback atomics and caps time bins",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(11, 0, 1000000, 0);
  trace.Submission(1000000, 0, true, false, 0);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&trace] {
      for (int j = 0; j < 100; ++j) trace.BufferEnded();
      trace.VoiceErrored();
    });
  }
  for (auto& thread : threads) thread.join();
  trace.Finish(602000000);
  CHECK(trace.GetTotal().buffer_ends_sampled == 400);
  CHECK(trace.GetTotal().voice_errors_sampled == 4);
  CHECK(trace.GetBin(AudioQueueTrace::kSeconds).buffer_ends_sampled == 400);
  CHECK(trace.GetBin(AudioQueueTrace::kSeconds).first_us == 602000000);
}

TEST_CASE("Audio queue log budget bounds rows and summaries across drivers",
          "[audio-queue-trace]") {
  AudioQueueLogBudget budget(2, 1);
  CHECK(budget.TakeRow());
  CHECK(budget.TakeRow());
  CHECK_FALSE(budget.TakeRow());
  CHECK(budget.TakeSummary());
  CHECK_FALSE(budget.TakeSummary());
  AudioQueueTrace first(12, 0, 1000000, 0);
  AudioQueueTrace reused_slot(13, 0, 2000000, 0);
  CHECK(first.driver_id() != reused_slot.driver_id());
  // A long idle setup must not spend the trace's first 600 seconds.
  reused_slot.Submission(700000000, 0, true, false, 0);
  CHECK(reused_slot.GetBin(0).attempts == 1);
  CHECK(reused_slot.start_us() == 700000000);
}

}  // namespace xe::apu::test
