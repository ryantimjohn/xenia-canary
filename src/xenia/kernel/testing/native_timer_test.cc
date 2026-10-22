// SDK-derived private fork research. Exclude from upstream contributions.
#include "xenia/kernel/native_timer_state.h"

#include <atomic>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/kernel/xthread.h"

#if XE_PLATFORM_WIN32
namespace xe::kernel::test {
using namespace std::chrono_literals;
namespace {
struct Fixture {
  X_KTIMER timer{};
  bool identity = true;  // Accessed only under global, as in production.
  std::shared_ptr<NativeTimerState> state;
  explicit Fixture(bool manual = false, NativeTimerState::Publish publish = {}) {
    auto event = manual ? threading::Event::CreateManualResetEvent(false)
                        : threading::Event::CreateAutoResetEvent(false);
    REQUIRE(event);
    state = std::make_shared<NativeTimerState>(
        std::move(event), [this] { return identity ? &timer : nullptr; }, publish);
  }
  ~Fixture() {
    state->Invalidate();
    state->Drain();
  }
};

struct ManualQueue {
  std::vector<std::function<void(void*)>> callbacks;
  NativeTimerState::Publish publisher() {
    return [this](std::function<void(void*)> callback,
                  NativeTimerState::Clock::time_point) {
      callbacks.push_back(std::move(callback));
      return std::weak_ptr<NativeTimerState::Item>{};
    };
  }
};

// Real event semantics, with a controllable failure of only the optional query.
class QueryEvent final : public threading::Event {
 public:
  QueryEvent() : event_(CreateAutoResetEvent(false)) {}
  void* native_handle() const override { return event_->native_handle(); }
  void Set() override { event_->Set(); }
  void Reset() override { event_->Reset(); }
  void Pulse() override { event_->Pulse(); }
  void SetBoostPriority() override { event_->SetBoostPriority(); }
  threading::EventInfo Query() override { return event_->Query(); }
  bool TryQuery(threading::EventInfo* info) override {
    return !fail && event_->TryQuery(info);
  }
  bool fail = false;
 private:
  std::unique_ptr<threading::Event> event_;
};
}  // namespace

TEST_CASE("Private native timer relative duration bounds", "[native_timer]") {
  using Clock = NativeTimerState::Clock;
  const auto now = Clock::time_point(1s);
  Clock::time_point deadline;
  REQUIRE(NativeTimerState::RelativeDeadline(-150000000, now, &deadline));
  CHECK(deadline - now == 15s);
  CHECK_FALSE(NativeTimerState::RelativeDeadline(0, now, &deadline));
  CHECK_FALSE(NativeTimerState::RelativeDeadline(1, now, &deadline));
  CHECK_FALSE(NativeTimerState::RelativeDeadline(
      std::numeric_limits<int64_t>::min(), now, &deadline));
  const auto edge = Clock::time_point::max() - 1ms;
  CHECK(NativeTimerState::RelativeDeadline(-10000, edge, &deadline));
  CHECK_FALSE(NativeTimerState::RelativeDeadline(-10001, edge, &deadline));
}

TEST_CASE("Private native timer really blocks and wakes WaitAny index two",
          "[native_timer]") {
  Fixture fixture;
  auto first = threading::Event::CreateManualResetEvent(false);
  auto second = threading::Event::CreateAutoResetEvent(false);
  threading::WaitHandle* handles[] = {first.get(), second.get(),
                                     fixture.state->wait_handle()};
  auto waiter = std::async(std::launch::async, [&] {
    return threading::WaitAny(handles, 3, false, 2s);
  });
  const bool blocked = waiter.wait_for(10ms) == std::future_status::timeout;
  bool previous = true;
  const bool armed = fixture.state->Arm(-100000, &previous);
  const auto result = waiter.get();  // Host wait itself has a finite timeout.
  CHECK(blocked);
  CHECK(armed);
  CHECK_FALSE(previous);
  CHECK(result.first == threading::WaitResult::kSuccess);
  CHECK(result.second == 2);
  CHECK(fixture.state->AfterWait());
  CHECK(uint32_t(fixture.timer.header.inserted) == 0);
  CHECK(uint32_t(fixture.timer.header.signal_state) == 0);
}

TEST_CASE("Private native timer manual and automatic signal consumption",
          "[native_timer]") {
  for (bool manual : {false, true}) {
    ManualQueue queue;
    Fixture fixture(manual, queue.publisher());
    bool previous;
    REQUIRE(fixture.state->Arm(-10000, &previous));
    queue.callbacks[0](nullptr);
    CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
          threading::WaitResult::kSuccess);
    REQUIRE(fixture.state->AfterWait());
    CHECK(uint32_t(fixture.timer.header.signal_state) == (manual ? 1 : 0));
    CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
          (manual ? threading::WaitResult::kSuccess
                  : threading::WaitResult::kTimeout));
  }
}

TEST_CASE("Private native timer cancellation and stale rearm callbacks",
          "[native_timer]") {
  ManualQueue queue;
  Fixture fixture(false, queue.publisher());
  bool previous;
  REQUIRE(fixture.state->Arm(-10000, &previous));
  CHECK_FALSE(previous);
  REQUIRE(fixture.state->Arm(-20000, &previous));
  CHECK(previous);
  queue.callbacks[0](nullptr);
  CHECK(uint32_t(fixture.timer.header.inserted) == 1);
  CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
        threading::WaitResult::kTimeout);
  REQUIRE(fixture.state->Cancel(&previous));
  CHECK(previous);
  REQUIRE(fixture.state->Cancel(&previous));
  CHECK_FALSE(previous);
  queue.callbacks[1](nullptr);
  CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
        threading::WaitResult::kTimeout);
  REQUIRE(fixture.state->Arm(-10000, &previous));
  queue.callbacks[2](nullptr);
  REQUIRE(fixture.state->Cancel(&previous));
  CHECK_FALSE(previous);  // Expired, but cancellation preserves its signal.
  CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
        threading::WaitResult::kSuccess);
}

TEST_CASE("Private native timer expiry before queue publication is retained",
          "[native_timer]") {
  Fixture fixture(false, [](std::function<void(void*)> callback,
                            NativeTimerState::Clock::time_point) {
    callback(nullptr);
    return std::weak_ptr<NativeTimerState::Item>{};
  });
  bool previous;
  REQUIRE(fixture.state->Arm(-10000, &previous));
  CHECK(uint32_t(fixture.timer.header.inserted) == 0);
  CHECK(threading::Wait(fixture.state->wait_handle(), false, 0ms) ==
        threading::WaitResult::kSuccess);
}

TEST_CASE("Private native timer invalidation and reuse cannot update new header",
          "[native_timer]") {
  ManualQueue queue;
  Fixture old(false, queue.publisher());
  bool previous;
  REQUIRE(old.state->Arm(-10000, &previous));
  {
    auto lock = global_critical_region::AcquireDirect();
    old.identity = false;  // Inline reinit erased the association.
  }
  CHECK_FALSE(old.state->associated());
  queue.callbacks[0](nullptr);
  CHECK(uint32_t(old.timer.header.signal_state) == 0);
  CHECK_FALSE(old.state->Arm(-10000, &previous));
  old.state->Invalidate();
  old.timer.header.signal_state = 37;  // Reused storage belongs to a new epoch.
  queue.callbacks[0](nullptr);
  CHECK(uint32_t(old.timer.header.signal_state) == 37);
}

TEST_CASE("Private native timer shutdown drains an in-flight publication",
          "[native_timer]") {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::function<void(void*)> callback;
  Fixture fixture(false, [&](std::function<void(void*)> value,
                             NativeTimerState::Clock::time_point) {
    callback = std::move(value);
    entered.set_value();
    released.wait();
    return std::weak_ptr<NativeTimerState::Item>{};
  });
  auto publisher = std::async(std::launch::async, [&] {
    bool previous;
    return fixture.state->Arm(-10000, &previous);
  });
  auto entered_future = entered.get_future();
  const bool did_enter = entered_future.wait_for(2s) == std::future_status::ready;
  if (!did_enter) {
    release.set_value();
    publisher.get();
    REQUIRE(did_enter);
  }
  fixture.state->Invalidate();
  std::promise<void> drain_started;
  auto drain = std::async(std::launch::async, [&] {
    drain_started.set_value();
    fixture.state->Drain();
  });
  const bool started = drain_started.get_future().wait_for(2s) ==
                       std::future_status::ready;
  const bool draining = drain.wait_for(10ms) == std::future_status::timeout;
  release.set_value();  // Always release before checks that could unwind.
  CHECK(publisher.get());
  drain.get();
  CHECK(started);
  CHECK(draining);
  fixture.timer.header.signal_state = 19;
  callback(nullptr);
  CHECK(uint32_t(fixture.timer.header.signal_state) == 19);
}

TEST_CASE("Private native timer failed publication cannot claim successful arm",
          "[native_timer]") {
  Fixture fixture(false, [](std::function<void(void*)>,
                            NativeTimerState::Clock::time_point)
                             -> std::weak_ptr<NativeTimerState::Item> {
    throw std::runtime_error("synthetic publication failure");
  });
  bool previous;
  CHECK_FALSE(fixture.state->Arm(-10000, &previous));
  CHECK(uint32_t(fixture.timer.header.inserted) == 0);
  fixture.state->Invalidate();
  fixture.state->Drain();  // Publisher accounting must not remain in flight.
}

TEST_CASE("Private native timer query failure and old wait cannot erase expiry",
          "[native_timer]") {
  X_KTIMER timer{};
  ManualQueue queue;
  auto event = std::make_unique<QueryEvent>();
  auto* query = event.get();
  auto state = std::make_shared<NativeTimerState>(
      std::move(event), [&] { return &timer; }, queue.publisher());
  bool previous;
  REQUIRE(state->Arm(-10000, &previous));
  queue.callbacks[0](nullptr);
  REQUIRE(threading::Wait(state->wait_handle(), false, 0ms) ==
          threading::WaitResult::kSuccess);
  query->fail = true;
  CHECK_FALSE(state->AfterWait());
  CHECK(uint32_t(timer.header.signal_state) == 1);
  query->fail = false;
  REQUIRE(state->Arm(-10000, &previous));
  queue.callbacks[1](nullptr);  // New expiry before the old wait's callback.
  CHECK(state->AfterWait());
  CHECK(uint32_t(timer.header.signal_state) == 1);
  CHECK(threading::Wait(state->wait_handle(), false, 0ms) ==
        threading::WaitResult::kSuccess);
  state->Invalidate();
  state->Drain();
}

TEST_CASE("Private native timer queued weak callback does not retain storage",
          "[native_timer]") {
  ManualQueue queue;
  X_KTIMER timer{};
  std::weak_ptr<NativeTimerState> weak;
  {
    auto state = std::make_shared<NativeTimerState>(
        threading::Event::CreateAutoResetEvent(false), [&] { return &timer; },
        queue.publisher());
    weak = state;
    bool previous;
    REQUIRE(state->Arm(-10000, &previous));
    state->Invalidate();
  }
  REQUIRE(weak.expired());
  timer.header.signal_state = 13;
  queue.callbacks[0](nullptr);
  CHECK(uint32_t(timer.header.signal_state) == 13);
}

TEST_CASE("Private native timer production association rejects reinitialized header",
          "[native_timer]") {
  X_KTIMER timer{};
  timer.header.type = TimerSynchronizationObject;
  timer.header.wait_list.flink_ptr = kXObjSignature;
  timer.header.wait_list.blink_ptr = 0x1234;
  REQUIRE(NativeTimerAssociationMatches(timer, TimerSynchronizationObject,
                                        0x1234));
  CHECK_FALSE(NativeTimerAssociationMatches(timer, TimerNotificationObject,
                                            0x1234));
  CHECK_FALSE(NativeTimerAssociationMatches(timer, TimerSynchronizationObject,
                                            0x5678));
  timer.header.wait_list.flink_ptr = 0;  // Inline KeInitialize erased signature.
  CHECK_FALSE(NativeTimerAssociationMatches(timer, TimerSynchronizationObject,
                                            0x1234));
  timer.header.wait_list.flink_ptr = kXObjSignature;
  timer.header.type = TimerNotificationObject;
  CHECK_FALSE(NativeTimerAssociationMatches(timer, TimerSynchronizationObject,
                                            0x1234));
  CHECK(NativeTimerAssociationMatches(timer, TimerNotificationObject, 0x1234));
}

TEST_CASE("Private native timer quiesce waits for an executing callback",
          "[native_timer]") {
  ManualQueue queue;
  X_KTIMER timer{};
  std::atomic<bool> hold{false};
  std::promise<void> entered, release, closing;
  auto released = release.get_future().share();
  auto state = std::make_shared<NativeTimerState>(
      threading::Event::CreateAutoResetEvent(false), [&]() -> X_KTIMER* {
        if (hold.load()) {
          entered.set_value();
          released.wait();  // Test-only controlled callback inside global.
        }
        return &timer;
      }, queue.publisher());
  bool previous;
  REQUIRE(state->Arm(-10000, &previous));
  hold = true;
  auto callback = std::async(std::launch::async,
                             [&] { queue.callbacks[0](nullptr); });
  const bool did_enter = entered.get_future().wait_for(2s) ==
                         std::future_status::ready;
  auto quiesce = std::async(std::launch::async, [&] {
    closing.set_value();
    state->Invalidate();
    state->Drain();
  });
  const bool did_close = closing.get_future().wait_for(2s) ==
                         std::future_status::ready;
  const bool waiting = quiesce.wait_for(10ms) == std::future_status::timeout;
  release.set_value();
  callback.get();
  quiesce.get();
  CHECK(did_enter);
  CHECK(did_close);
  CHECK(waiting);
  timer.header.signal_state = 23;
  queue.callbacks[0](nullptr);
  CHECK(uint32_t(timer.header.signal_state) == 23);
}
}  // namespace xe::kernel::test
#endif  // XE_PLATFORM_WIN32
