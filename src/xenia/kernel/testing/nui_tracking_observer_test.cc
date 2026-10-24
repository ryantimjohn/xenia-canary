/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_tracking_observer.h"

#include <limits>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::xam::test {

TEST_CASE("HUD sample bounds confidence and checks tracked vectors",
          "[nui-tracking-observer]") {
  NuiMsg2B002 guest{};
  guest.timestamp = 123;
  guest.skeletons[0].tracking_state = 2;
  guest.skeletons[0].quality_flags = 1;
  guest.skeletons[0].position_tracking_state[0] = 0;
  guest.skeletons[0].position_tracking_state[1] = 1;
  guest.skeletons[0].position_tracking_state[2] = 2;
  guest.skeletons[0].position_tracking_state[3] = 99;
  guest.skeletons[0].skeleton_positions[1].x = 1.0f;
  guest.skeletons[0].skeleton_positions[2].x =
      std::numeric_limits<float>::quiet_NaN();
  guest.skeletons[1].tracking_state = 1;
  guest.skeletons[1].position_tracking_state[0] = 99;
  guest.engaged_tracking_id[0] = 77;  // [out], not evidence of selection.
  const auto sample = NuiTrackingObserver::ReadHudSample(guest);
  CHECK(sample.guest_timestamp_ms == 123);
  CHECK(sample.joint_states[0] == 17);
  CHECK(sample.joint_states[1] == 1);
  CHECK(sample.joint_states[2] == 1);
  CHECK(sample.joint_states[3] == 1);
  CHECK(sample.quality_nonzero == 1);
  CHECK(sample.nonfinite_vectors == 1);
  CHECK(sample.zero_vectors == 18);
  CHECK_FALSE(sample.guide_took);
  CHECK(sample.guide_selected_id == 0);
}

TEST_CASE("Disabled tracking observer does no collection",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(false);
  NuiTrackingObserver::HudSample hud;
  hud.states[0] = 2;
  hud.ids[0] = 91;
  observer.SourceFrame(7, 1000000);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCopied, 1000000,
                         999000, 42);
  observer.Hud(hud, 1000000);
  CHECK_FALSE(observer.TakeRow(3000000));
  CHECK(observer.Summary().source_frames == 0);
  CHECK(observer.Summary().hud_frames == 0);
}

TEST_CASE("Tracking observer distinguishes delivery and HUD evidence",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  observer.SourceFrame(1, 1000000);
  observer.SourceFrame(4, 1100000);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kNoBuffer, 1100000);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCopied, 1200000,
                         1100000, 42);
  NuiTrackingObserver::HudSample hud;
  hud.guest_timestamp_ms = 42;
  hud.states[0] = 2;
  hud.ids[0] = 91;
  hud.guide_took = true;
  hud.guide_selected_id = 91;
  hud.joint_states = {1, 2, 17, 0};
  hud.quality_nonzero = 1;
  hud.nonfinite_vectors = 1;
  observer.Hud(hud, 1201000);  // May be nested in the completion callback.
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCompletionCalled,
                         1200000);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kReturned, 1202000);
  hud.ids[0] = 92;
  hud.guide_selected_id = 91;
  hud.guest_timestamp_ms = 999;
  observer.Hud(hud, 1300000);
  const auto row = observer.TakeRow(2000000);
  REQUIRE(row);
  const auto& s = row->stats;
  CHECK(row->row_index == 1);
  CHECK(row->start_us == 1000000);
  CHECK(row->end_us == 2000000);
  CHECK(s.source_frames == 2);
  CHECK(s.source_skipped == 2);
  CHECK(s.no_buffer == 1);
  CHECK(s.copied_stamped == 1);
  CHECK(s.completion_called == 1);
  CHECK(s.callback_returned == 1);
  CHECK(s.host_age_max_ms == 100);
  CHECK(s.timestamp_exact == 1);
  CHECK(s.timestamp_unmatched == 1);
  CHECK(s.id_appeared == 1);
  CHECK(s.id_replaced == 1);
  CHECK(s.guide_selected_matched == 1);
  CHECK(s.guide_selected_unmatched == 1);
  CHECK(s.joint_states[3] == 0);
  CHECK(s.quality_nonzero == 2);
  CHECK(s.nonfinite_vectors == 2);
}

TEST_CASE("Tracking observer bounds rows and handles ambiguous stamps",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCopied, 1000000, 0,
                         42);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCopied, 1000001,
                         2000000, 42);
  NuiTrackingObserver::HudSample hud;
  hud.guest_timestamp_ms = 42;
  hud.states[0] = 9;
  hud.joint_states[3] = 20;
  observer.Hud(hud, 1000002);
  CHECK(observer.Summary().timestamp_ambiguous == 1);
  CHECK(observer.Summary().slot_states[3] == 1);
  CHECK(observer.Summary().host_age_unknown == 2);
  for (uint64_t i = 1; i <= 361; ++i) {
    observer.SourceFrame(i, 1000000 + i * 1000000);
    auto row = observer.TakeRow(1000000 + i * 1000000);
    CHECK(bool(row) == (i <= 360));
  }
  observer.SourceFrame(1, 363000000);
  CHECK(observer.Summary().source_frames == 362);
  CHECK(observer.Summary().source_resets == 1);
}

TEST_CASE("Tracking observer shares bounded counters across threads",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 4; ++i) {
    threads.emplace_back([&observer] {
      NuiTrackingObserver::HudSample hud;
      for (unsigned j = 0; j < 100; ++j) {
        observer.Hud(hud, 1000000);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(observer.Summary().hud_frames == 400);
}

TEST_CASE("Tracking observer clears matching history between feed sessions",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  observer.SessionStarted(1000000);
  observer.SourceFrame(100, 1000000);
  observer.DepthDelivery(NuiTrackingObserver::Delivery::kCopied, 1000000,
                         999000, 42);
  NuiTrackingObserver::HudSample hud;
  hud.guest_timestamp_ms = 42;
  hud.ids[0] = 12;
  observer.Hud(hud, 1001000);
  observer.SessionStarted(2000000);
  observer.SourceFrame(1, 2000000);
  hud.ids[0] = 13;
  observer.Hud(hud, 2001000);
  const auto total = observer.Summary();
  CHECK(total.sessions == 2);
  CHECK(total.source_resets == 0);
  CHECK(total.id_appeared == 2);
  CHECK(total.id_replaced == 0);
  CHECK(total.timestamp_exact == 1);
  CHECK(total.timestamp_unmatched == 1);
}

TEST_CASE("Tracking observer distinguishes slot moves from tracked set changes",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  NuiTrackingObserver::HudSample hud;
  hud.states[0] = hud.states[1] = 2;
  hud.ids[0] = 12;
  hud.ids[1] = 13;
  observer.Hud(hud, 1000000);
  hud.ids[0] = 13;
  hud.ids[1] = 12;
  observer.Hud(hud, 1001000);
  CHECK(observer.Summary().same_set_slot_reassigned == 1);
  CHECK(observer.Summary().tracked_set_changed == 0);
  hud.ids[1] = 14;
  observer.Hud(hud, 1002000);
  CHECK(observer.Summary().same_set_slot_reassigned == 1);
  CHECK(observer.Summary().tracked_set_changed == 1);
}

TEST_CASE("Tracking observer labels delayed intervals and caps feed-end logs",
          "[nui-tracking-observer]") {
  NuiTrackingObserver observer(true);
  observer.SessionStarted(1000000);
  observer.SourceFrame(1, 1000000);
  const auto delayed = observer.TakeRow(4500000);
  REQUIRE(delayed);
  CHECK(delayed->row_index == 1);
  CHECK(delayed->start_us == 1000000);
  CHECK(delayed->end_us == 4500000);
  const auto first_end = observer.TakeFirstFeedEnd(5000000);
  REQUIRE(first_end);
  CHECK(first_end->row_index == 0);
  CHECK(first_end->start_us == 1000000);
  CHECK(first_end->end_us == 5000000);
  CHECK(first_end->stats.sessions == 1);
  observer.SessionStarted(6000000);
  observer.SourceFrame(2, 6000000);
  CHECK_FALSE(observer.TakeFirstFeedEnd(7000000));
  CHECK(observer.Summary().sessions == 2);
  CHECK(observer.Summary().source_frames == 2);
  CHECK(first_end->stats.source_frames == 1);
}

}  // namespace xe::kernel::xam::test
