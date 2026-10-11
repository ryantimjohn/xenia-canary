/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/depth_frame_pool.h"

namespace xe {
namespace nui {

namespace {

std::shared_ptr<DepthFrame> MakeFrame() {
  auto frame = std::make_shared<DepthFrame>();
  frame->depth_mm.resize(static_cast<size_t>(DepthFrame::kWidth) *
                         DepthFrame::kHeight);
  return frame;
}

}  // namespace

std::shared_ptr<DepthFrame> DepthFramePool::Acquire() {
  for (auto& slot : frames_) {
    if (!slot) {
      slot = MakeFrame();
      return slot;
    }
    // Only the pool is holding this one, so nobody can see us rewrite it.
    if (slot.use_count() == 1) {
      return slot;
    }
  }
  // Every pooled frame is still being read somewhere. Give this one its own
  // storage rather than stalling the backend.
  return MakeFrame();
}

}  // namespace nui
}  // namespace xe
