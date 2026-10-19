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
  CHECK(budget.TakeSummary());  // Reserve before emitting this driver's rows.
  CHECK(budget.TakeRow());
  CHECK(budget.TakeRow());
  CHECK_FALSE(budget.TakeRow());
  CHECK_FALSE(budget.TakeSummary());
  AudioQueueTrace first(12, 0, 1000000, 0);
  AudioQueueTrace reused_slot(13, 0, 2000000, 0);
  CHECK(first.driver_id() != reused_slot.driver_id());
  // A long idle setup must not spend the trace's first 600 seconds.
  reused_slot.Submission(700000000, 0, true, false, 0);
  CHECK(reused_slot.GetBin(0).attempts == 1);
  CHECK(reused_slot.start_us() == 700000000);
}

TEST_CASE("Audio queue selection keeps a late severe bin over early healthy bins",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(14, 0, 1000000, 0);
  for (uint64_t i = 0; i < 10; ++i) {
    trace.Resume();  // Each interval starts with a fresh priming submission.
    trace.Submission(1000000 + i * 1000000, 0, true, false, 0);
  }
  trace.Resume();
  trace.Submission(600000000, 0, true, false, 0);
  trace.Submission(600030000, 0, true, false, 0);
  const auto selected = trace.SelectWorstBins();
  CHECK(selected.occupied == 11);
  CHECK(selected.count == AudioQueueTrace::kRowsPerShutdown);
  bool found_late = false;
  for (size_t i = 0; i < selected.count; ++i)
    found_late |= selected.indexes[i] == 599;
  CHECK(found_late);
  CHECK(selected.indexes[0] == 599);
  CHECK(selected.occupied - selected.count == 3);
}

TEST_CASE("Audio queue summary locates the largest gap omitted from top eight",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(15, 0, 1000000, 0);
  for (uint64_t i = 0; i < 9; ++i) {
    trace.Resume();
    const uint64_t at = 1000000 + i * 1000000;
    trace.Submission(at, 0, true, false, 0);
    trace.Submission(at + 1, 0, true, false, 0);
  }
  trace.Resume();
  trace.Submission(21000000, 1, true, false, 0);
  trace.Submission(21050000, 1, true, false, 0);
  const auto selected = trace.SelectWorstBins();
  CHECK(selected.occupied == 10);
  CHECK(selected.count == AudioQueueTrace::kRowsPerShutdown);
  CHECK(selected.max_success_gap_us == 50000);
  CHECK(selected.max_success_gap_bin == 20);
  bool gap_bin_selected = false;
  for (size_t i = 0; i < selected.count; ++i)
    gap_bin_selected |= selected.indexes[i] == 20;
  CHECK_FALSE(gap_bin_selected);
  CHECK(trace.GetTotal().max_success_gap_us == 50000);
  trace.Resume();
  trace.Submission(31000000, 1, true, false, 0);
  AudioQueueCheckpointBudget checkpoint_budget;
  AudioQueueTrace::Checkpoint checkpoint;
  CHECK(trace.TakeCheckpoint(31000000, checkpoint_budget, &checkpoint));
  CHECK(checkpoint.max_success_gap_bin == 20);
  CHECK(checkpoint.total.max_success_gap_us == 50000);
  // The live checkpoint is available without Finish or driver Shutdown.
  CHECK(checkpoint.total.attempts == 21);
}

TEST_CASE("Audio queue checkpoints start at first submit and exact 30 seconds",
          "[audio-queue-trace]") {
  AudioQueueTrace trace(16, 0, 1000000, 0);
  AudioQueueCheckpointBudget budget;
  AudioQueueTrace::Checkpoint checkpoint;
  trace.Submission(5000000, 0, true, false, 0);
  CHECK_FALSE(trace.TakeCheckpoint(5000000, budget, &checkpoint));
  trace.Submission(34999999, 1, true, false, 0);
  CHECK_FALSE(trace.TakeCheckpoint(34999999, budget, &checkpoint));
  trace.Submission(35000000, 1, true, false, 0);
  CHECK(trace.TakeCheckpoint(35000000, budget, &checkpoint));
  CHECK(checkpoint.ordinal == 1);
  CHECK(checkpoint.sample_us == 35000000);
  CHECK(checkpoint.total.attempts == 3);
  trace.Submission(64999999, 1, true, false, 0);
  CHECK_FALSE(trace.TakeCheckpoint(64999999, budget, &checkpoint));
  trace.Submission(65000000, 1, true, false, 0);
  CHECK(trace.TakeCheckpoint(65000000, budget, &checkpoint));
  CHECK(checkpoint.ordinal == 2);
  CHECK(checkpoint.total.attempts == 5);
  AudioQueueTrace paused(17, 0, 1000000, 0);
  AudioQueueCheckpointBudget paused_budget;
  paused.Submission(1000000, 0, true, false, 0);
  paused.Pause();
  paused.Submission(31000000, 1, true, false, 0);
  CHECK_FALSE(paused.TakeCheckpoint(31000000, paused_budget, &checkpoint));
  paused.Resume();
  paused.Submission(31000001, 1, true, false, 0);
  CHECK(paused.TakeCheckpoint(31000001, paused_budget, &checkpoint));
  CHECK(checkpoint.ordinal == 1);
}

TEST_CASE("Audio queue live checkpoint budgets bound driver and process",
          "[audio-queue-trace]") {
  AudioQueueCheckpointBudget process_budget;
  uint32_t emitted = 0;
  for (uint64_t driver = 0; driver < 9; ++driver) {
    AudioQueueTrace trace(20 + driver, 0, 1000000, 0);
    AudioQueueTrace::Checkpoint checkpoint;
    trace.Submission(1000000, 0, true, false, 0);
    for (uint64_t ordinal = 1; ordinal <= 13; ++ordinal) {
      const uint64_t at = 1000000 +
                          ordinal * AudioQueueTrace::kCheckpointPeriodUs;
      trace.Submission(at, 1, true, false, 0);
      const bool took = trace.TakeCheckpoint(at, process_budget, &checkpoint);
      CHECK(took == (driver < 8 && ordinal <= 12));
      if (took) {
        ++emitted;
        CHECK(checkpoint.ordinal == ordinal);
      }
    }
  }
  CHECK(emitted == 96);
  AudioQueueTrace late(40, 0, 1000000, 0);
  AudioQueueCheckpointBudget empty_budget(0);
  AudioQueueTrace::Checkpoint checkpoint;
  late.Submission(1000000, 0, true, false, 0);
  late.Submission(31000000, 1, true, false, 0);
  CHECK_FALSE(late.TakeCheckpoint(31000000, empty_budget, &checkpoint));
  AudioQueueCheckpointBudget one_token(1);
  CHECK(late.TakeCheckpoint(31000000, one_token, &checkpoint));
  CHECK(checkpoint.ordinal == 1);
}

}  // namespace xe::apu::test
