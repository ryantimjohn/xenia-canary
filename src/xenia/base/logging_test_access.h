// Copyright 2026 Xenia Canary. Released under the BSD license.
// Private fork-only capture shutdown test seam; never upstream SDK ancestry.
#ifndef XENIA_BASE_LOGGING_TEST_ACCESS_H_
#define XENIA_BASE_LOGGING_TEST_ACCESS_H_

#include <memory>
#include <string_view>

#include "xenia/base/logging.h"

namespace xe {
class Logger;
namespace logging::testing {
// Isolated real ring/writer/sink; never replaces the process logger. Stop and
// destruction require released test sinks and must not race other fixture
// calls.
class LoggerFixture {
 public:
  explicit LoggerFixture(std::unique_ptr<LogSink> sink);
  ~LoggerFixture();
  LoggerFixture(const LoggerFixture&) = delete;
  LoggerFixture& operator=(const LoggerFixture&) = delete;
  void Append(std::string_view line);
  LogDrainResult Drain(std::chrono::milliseconds timeout);
  uint64_t ReserveUnpublishedLine();
  void PublishReservedLine(uint64_t sequence);
  void Stop();

 private:
  Logger* logger_;
};
}  // namespace logging::testing
}  // namespace xe
#endif  // XENIA_BASE_LOGGING_TEST_ACCESS_H_
