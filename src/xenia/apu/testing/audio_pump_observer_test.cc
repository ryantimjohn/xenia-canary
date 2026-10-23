// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private diagnostic; never upstream with SDK-derived work.
#include "xenia/apu/audio_pump_observer.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::apu::pump_observer::test {

static Attempt MakeAttempt(uint64_t selected_us, uint64_t deadline_us,
                           Outcome outcome) {
  Attempt a;
  a.selected_now_us = selected_us;
  a.deadline_us = deadline_us;
  a.outcome = outcome;
  a.selected = {selected_us, selected_us * 10, 3, true};
  a.wake = {selected_us + 1000, selected_us * 10 + 10000, 4, true};
  return a;
}

TEST_CASE("Audio pump windows keep distinct wake and callback worst cases",
          "[audio-pump-observer]") {
  Collector collector;
  Report report;
  auto denied = MakeAttempt(1000000, 990000, Outcome::kTokenTimeout);
  denied.client_index = 0;
  CHECK_FALSE(collector.Observe(denied, &report));

  auto executed = MakeAttempt(2000000, 1999000, Outcome::kExecuted);
  executed.client_index = 1;
  executed.wake.steady_us = 2020000;
  executed.callback_begin = {2021000, 20210000, 5, true};
  executed.callback_end = {2046000, 20460000, 6, true};
  CHECK_FALSE(collector.Observe(executed, &report));

  auto next = MakeAttempt(6000000, 6000000, Outcome::kNoCallback);
  CHECK(collector.Observe(next, &report));
  CHECK(report.selected == 2);
  CHECK(report.outcomes[std::size_t(Outcome::kTokenTimeout)] == 1);
  CHECK(report.outcomes[std::size_t(Outcome::kExecuted)] == 1);
  CHECK(report.worst_wake.present);
  CHECK(report.worst_wake.attempt.client_index == 1);
  CHECK(report.worst_callback.present);
  CHECK(report.worst_callback.attempt.client_index == 1);
  CHECK(report.worst_token_timeout.present);
  CHECK(report.worst_token_timeout.attempt.client_index == 0);
  CHECK(report.explicit_qpc_queries == 12);  // Two, then four stamp pairs.
  CHECK(report.stamp_bracket_ticks == 25);
  CHECK_FALSE(report.cap_reached);
}

TEST_CASE("Audio pump window skips and final output cap are explicit",
          "[audio-pump-observer]") {
  Collector collector;
  Report report;
  CHECK_FALSE(collector.Observe(MakeAttempt(1, 1, Outcome::kNoCallback),
                                &report));
  CHECK(collector.Observe(
      MakeAttempt(3 * kWindowUs + 1, 3 * kWindowUs + 1,
                  Outcome::kNoCallback),
      &report));
  CHECK(report.selected == 1);
  CHECK(report.window_start_us == 1);
  CHECK(collector.reports_emitted() == 1);
  CHECK(collector.Observe(
      MakeAttempt(4 * kWindowUs + 1, 4 * kWindowUs + 1,
                  Outcome::kNoCallback),
      &report));
  CHECK(report.skipped_empty_windows == 2);
  for (uint32_t i = 5; i <= kMaxWindows + 2; ++i) {
    CHECK(collector.Observe(
        MakeAttempt(uint64_t(i) * kWindowUs + 1,
                    uint64_t(i) * kWindowUs + 1, Outcome::kNoCallback),
        &report));
  }
  CHECK(collector.reports_emitted() == kMaxWindows);
  CHECK(collector.exhausted());
  CHECK(report.cap_reached);
  CHECK_FALSE(collector.Observe(
      MakeAttempt(1000 * kWindowUs, 1000 * kWindowUs,
                  Outcome::kNoCallback),
      &report));
}

TEST_CASE("Audio pump initial zero deadline is not a lateness maximum",
          "[audio-pump-observer]") {
  Collector collector;
  Report report;
  auto first = MakeAttempt(1000000, 0, Outcome::kTokenTimeout);
  CHECK_FALSE(collector.Observe(first, &report));
  CHECK(collector.Observe(
      MakeAttempt(6000000, 6000000, Outcome::kNoCallback), &report));
  CHECK(report.selected == 1);
  CHECK(report.outcomes[std::size_t(Outcome::kTokenTimeout)] == 1);
  CHECK_FALSE(report.worst_selected.present);
  CHECK_FALSE(report.worst_wake.present);
  CHECK_FALSE(report.worst_token_timeout.present);
}

TEST_CASE("Audio pump lifecycle serials stop qualifying slot reuse at cap",
          "[audio-pump-observer]") {
  LifecycleBudget budget;
  const auto first = budget.Claim(true);
  const auto removed = budget.Claim(false);
  const auto reused = budget.Claim(true);
  const auto invalidated = budget.Claim(false);
  CHECK(first.emit);
  CHECK(first.serial == 1);
  CHECK(removed.emit);
  CHECK(removed.serial == 0);
  CHECK(reused.emit);
  CHECK(reused.serial == 2);
  CHECK(invalidated.emit);
  CHECK(invalidated.serial == 0);
  CHECK_FALSE(budget.identity_unknown());
  for (uint32_t i = 4; i < kMaxLifecycleRows; ++i) {
    CHECK(budget.Claim(false).emit);
  }
  const auto cutoff = budget.Claim(true);
  CHECK(cutoff.emit);
  CHECK(cutoff.cutoff);
  CHECK(budget.identity_unknown());
  CHECK_FALSE(budget.Claim(true).emit);
}

TEST_CASE("Audio pump window output budget is shared and finite",
          "[audio-pump-observer]") {
  WindowBudget budget;
  for (uint32_t i = 0; i < kMaxSessions; ++i) {
    CHECK(budget.ClaimSession());
  }
  CHECK_FALSE(budget.ClaimSession());
  for (uint32_t i = 0; i + 1 < kMaxWindows; ++i) {
    const auto claim = budget.Claim();
    CHECK(claim.emit);
    CHECK_FALSE(claim.final);
  }
  const auto final = budget.Claim();
  CHECK(final.emit);
  CHECK(final.final);
  CHECK(budget.exhausted());
  CHECK_FALSE(budget.Claim().emit);
}

}  // namespace xe::apu::pump_observer::test
