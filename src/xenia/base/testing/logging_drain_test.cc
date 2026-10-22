// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only capture shutdown tests; never upstream SDK ancestry.
#include "xenia/base/logging_test_access.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe::logging::testing {
namespace {
using namespace std::chrono_literals;

struct SinkState {
  std::mutex mutex;
  std::condition_variable changed;
  bool block_write = false, block_flush = false;
  bool write_entered = false, flush_entered = false, released = false;
  bool gate_expired = false;
  unsigned flushes = 0;
  std::string text;
  LoggerFixture* reenter = nullptr;
  LogDrainResult reentrant_result = LogDrainResult::kUnavailable;

  bool Entered(bool flush = false) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 2s, [&] {
      return flush ? flush_entered : write_entered;
    });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

class ControlledSink : public LogSink {
 public:
  explicit ControlledSink(SinkState& state) : state_(state) {}
  void Write(const char* bytes, size_t size) override {
    std::unique_lock<std::mutex> lock(state_.mutex);
    state_.write_entered = true;
    state_.changed.notify_all();
    if (state_.block_write &&
        !state_.changed.wait_for(lock, 5s, [&] { return state_.released; })) {
      state_.gate_expired = true;
    }
    state_.text.append(bytes, size);
  }
  void Flush() override {
    std::unique_lock<std::mutex> lock(state_.mutex);
    state_.flush_entered = true;
    state_.changed.notify_all();
    if (state_.block_flush &&
        !state_.changed.wait_for(lock, 5s, [&] { return state_.released; })) {
      state_.gate_expired = true;
    }
    if (state_.reenter) {
      state_.reentrant_result = state_.reenter->Drain(100ms);
    }
    ++state_.flushes;
  }

 private:
  SinkState& state_;
};
}  // namespace

TEST_CASE("Capture log drain waits for pending output", "[logging_drain]") {
  SinkState state;
  state.block_write = true;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  logger.Append("positive close ACK");
  const bool entered = state.Entered();
  std::promise<void> started;
  auto ready = started.get_future();
  auto drain = std::async(std::launch::async, [&] {
    started.set_value();
    return logger.Drain(2s);
  });
  const bool began = ready.wait_for(1s) == std::future_status::ready;
  const bool premature = drain.wait_for(20ms) == std::future_status::ready;
  state.Release();  // Always release before assertions or async destruction.
  const auto result = drain.get();
  logger.Stop();
  REQUIRE(entered);
  REQUIRE(began);
  REQUIRE_FALSE(premature);
  REQUIRE(result == LogDrainResult::kComplete);
  REQUIRE(state.text == "positive close ACK\n\n");
  REQUIRE(state.flushes > 0);
  REQUIRE_FALSE(state.gate_expired);
}

TEST_CASE("Capture log drain cannot acknowledge a blocked sink flush",
          "[logging_drain]") {
  SinkState state;
  state.block_flush = true;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  logger.Append("pending");
  const auto first = logger.Drain(30ms);
  const bool entered = state.Entered(true);
  state.Release();
  const auto later = logger.Drain(2s);
  logger.Stop();
  REQUIRE(first == LogDrainResult::kTimedOut);
  REQUIRE(entered);
  REQUIRE(later == LogDrainResult::kComplete);
  REQUIRE(state.text == "pending\n\n");
  REQUIRE_FALSE(state.gate_expired);
}

TEST_CASE("Capture log drain times out without reserving a full ring",
          "[logging_drain]") {
  SinkState state;
  state.block_write = true;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  logger.Append("x");
  const bool entered = state.Entered();
  // Each tiny line is one 256-byte block. With the first writer blocked before
  // consumer publication, exactly 32767 remaining blocks fit the real 8MiB ring.
  // Skip filling if entry failed, so a failing test never blocks an Append.
  if (entered) {
    for (unsigned i = 1; i < 32768; ++i) logger.Append("x");
  }
  const auto result = entered ? logger.Drain(30ms) : LogDrainResult::kUnavailable;
  state.Release();
  logger.Stop();
  REQUIRE(entered);
  REQUIRE(result == LogDrainResult::kTimedOut);
  REQUIRE(state.text.size() == 65537);
  REQUIRE_FALSE(state.gate_expired);
}

TEST_CASE("Capture log drain refuses a stopped writer", "[logging_drain]") {
  SinkState state;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  logger.Stop();
  REQUIRE(logger.Drain(100ms) == LogDrainResult::kUnavailable);
  REQUIRE(state.text == "\n");  // Existing termination record, no flush marker.
}

TEST_CASE("Capture log drain cannot skip an unpublished predecessor",
          "[logging_drain]") {
  SinkState state;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  const uint64_t held = logger.ReserveUnpublishedLine();
  logger.Append("after gap");
  const auto first = logger.Drain(30ms);
  logger.PublishReservedLine(held);  // Release before all assertions/joins.
  const auto later = logger.Drain(2s);
  logger.Stop();
  REQUIRE(first == LogDrainResult::kTimedOut);
  REQUIRE(later == LogDrainResult::kComplete);
  REQUIRE(state.text == "\nafter gap\n\n");
}

TEST_CASE("Capture log drain refuses writer thread reentry",
          "[logging_drain]") {
  SinkState state;
  LoggerFixture logger(std::make_unique<ControlledSink>(state));
  state.reenter = &logger;
  logger.Append("reentrant");
  const auto result = logger.Drain(2s);
  logger.Stop();
  REQUIRE(result == LogDrainResult::kComplete);
  REQUIRE(state.reentrant_result == LogDrainResult::kWriterThread);
}
}  // namespace xe::logging::testing
