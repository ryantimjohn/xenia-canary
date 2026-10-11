/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_DEPTH_RECORDING_H_
#define XENIA_NUI_DEPTH_RECORDING_H_

#include <cstdint>

namespace xe {
namespace nui {

// On-disk form of a host depth stream, a ".xdepth" file: one file header
// followed by the frames a NuiSystem published, each a frame header and its
// depth samples, appended in the order they were published. The file is valid
// after any number of complete frames, so a reader stops at the first frame it
// cannot read whole and a recording outlives an emulator that was killed
// halfway through a write.
//
// The fields are host endian, which is little endian on every platform xenia
// supports. None of this ever reaches guest memory, so none of it is xe::be.

// The eight bytes every recording starts with.
constexpr char kDepthRecordingMagic[8] = {'X', 'N', 'U', 'I',
                                          'D', 'E', 'P', '1'};

struct DepthRecordingHeader {
  char magic[8];
  // Geometry of every frame in the file, 640x480 for a Kinect sensor.
  uint32_t width;
  uint32_t height;
  uint32_t reserved[2];
};
static_assert(sizeof(DepthRecordingHeader) == 24,
              "The recording header is written to disk as it is laid out.");

struct DepthRecordingFrameHeader {
  // The sequence number and capture time the frame was published with.
  uint64_t sequence;
  uint64_t timestamp_us;
  // Followed by width * height samples in millimetres, row major, 0 meaning
  // the sensor had no reading for that pixel.
};
static_assert(sizeof(DepthRecordingFrameHeader) == 16,
              "The frame header is written to disk as it is laid out.");

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_DEPTH_RECORDING_H_
