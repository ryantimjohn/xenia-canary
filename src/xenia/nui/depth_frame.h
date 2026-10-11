/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_DEPTH_FRAME_H_
#define XENIA_NUI_DEPTH_FRAME_H_

#include <cstdint>
#include <vector>

namespace xe {
namespace nui {

// One depth image captured by a NUI (Kinect) sensor. A frame is immutable once
// it has been handed to NuiSystem::PublishFrame, so readers may keep a
// shared_ptr to it for as long as they like.
struct DepthFrame {
  static constexpr uint32_t kWidth = 640;
  static constexpr uint32_t kHeight = 480;

  // Index of this frame in the backend's stream. Starts at 1 and increases by
  // exactly 1 per published frame. 0 means never published.
  uint64_t sequence = 0;
  // Capture time taken from std::chrono::steady_clock, in microseconds.
  uint64_t timestamp_us = 0;
  uint32_t width = kWidth;
  uint32_t height = kHeight;
  // width * height samples in millimetres, host endian, row-major. A sample of
  // 0 means the sensor had no reading for that pixel.
  //
  // Each row runs in the orientation the Kinect for Windows runtime delivers
  // its 640x480 depth in, which is what the recordings hold and what the depth
  // preview draws. It is not the orientation the guest wants: the title's
  // runtime expects each row reversed, so the guest feed writes the rows
  // mirrored (ConvertDepthFrame's mirror_columns) and this frame stays as it
  // is. On a live sensor, feeding the rows as they are here made the title's
  // hand cursor move against the hand, and its raft game did not answer a
  // sideways lean.
  std::vector<uint16_t> depth_mm;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_DEPTH_FRAME_H_
