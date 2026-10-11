/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_DEPTH_FRAME_POOL_H_
#define XENIA_NUI_DEPTH_FRAME_POOL_H_

#include <array>
#include <cstddef>
#include <memory>

#include "xenia/nui/depth_frame.h"

namespace xe {
namespace nui {

// A handful of depth frames that a backend hands to NuiSystem::PublishFrame
// over and over, instead of allocating 600 KiB for every frame a sensor
// produces thirty times a second.
//
// A published frame stays readable for as long as anyone holds a reference to
// it - the guest's frame feed, the preview overlay and the recorder's write
// all keep one for a while - so a pooled frame is only ever handed out again
// once the pool is the last owner left. When every pooled frame is still being
// read somewhere, Acquire() allocates a fresh one rather than stalling the
// backend or rewriting a frame under its readers.
//
// Not thread safe: a backend's frames all come from its own worker thread.
class DepthFramePool {
 public:
  // How many frames the pool cycles through.
  static constexpr size_t kSize = 3;

  // Returns a 640x480 frame that nobody else is reading. It holds whatever the
  // last user of that frame left in it, so the caller fills it in whole.
  std::shared_ptr<DepthFrame> Acquire();

 private:
  std::array<std::shared_ptr<DepthFrame>, kSize> frames_;
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_DEPTH_FRAME_POOL_H_
