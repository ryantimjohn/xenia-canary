// Copyright 2026 Xenia Canary. Released under the BSD license - see LICENSE.
#ifndef XENIA_HID_REPLAY_REPLAY_INPUT_DRIVER_H_
#define XENIA_HID_REPLAY_REPLAY_INPUT_DRIVER_H_

#include <array>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace xe::hid::replay {

struct Event {
  uint64_t time_us = 0;
  uint32_t port = 0;
  X_RESULT status = X_ERROR_DEVICE_NOT_CONNECTED;
  X_INPUT_STATE state = {};
};
struct Recording {
  uint64_t duration_us = 0;
  std::vector<Event> events;
};
// Decodes explicit little-endian fields, never native guest structs.
bool ParseRecording(const std::vector<uint8_t>& bytes, Recording* out,
                    std::string* error);

class ReplayInputDriver final : public InputDriver {
 public:
  using Clock = std::function<uint64_t()>;  // Monotonic wall microseconds.
  ReplayInputDriver(ui::Window* window, size_t z_order,
                    std::filesystem::path path, Clock clock = {});
  X_STATUS Setup() override;
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override { return InputType::Controller; }

 private:
  // All public polling paths share this lock, including first-poll clock arming.
  void Advance(bool start);
  std::filesystem::path path_;
  Clock clock_;
  std::mutex mutex_;
  Recording recording_;
  std::array<Event, 4> states_ = {};
  size_t next_ = 0;
  uint64_t start_us_ = 0;
  bool loaded_ = false;
  bool started_ = false;
  bool completed_ = false;
};

}  // namespace xe::hid::replay
#endif  // XENIA_HID_REPLAY_REPLAY_INPUT_DRIVER_H_
