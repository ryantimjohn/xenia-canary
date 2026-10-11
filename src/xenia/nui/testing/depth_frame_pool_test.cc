/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/depth_frame_pool.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "xenia/nui/depth_frame.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace nui {
namespace test {

TEST_CASE("DepthFramePool hands out 640x480 frames",
          "[nui][depth_frame_pool]") {
  DepthFramePool pool;
  const auto frame = pool.Acquire();
  REQUIRE(frame != nullptr);
  CHECK(frame->width == DepthFrame::kWidth);
  CHECK(frame->height == DepthFrame::kHeight);
  CHECK(frame->depth_mm.size() ==
        static_cast<size_t>(DepthFrame::kWidth) * DepthFrame::kHeight);
}

TEST_CASE("DepthFramePool recycles a frame its readers have let go",
          "[nui][depth_frame_pool]") {
  DepthFramePool pool;
  auto frame = pool.Acquire();
  REQUIRE(frame != nullptr);
  const DepthFrame* first = frame.get();

  // The pool is the only owner left, so nobody can see it being rewritten and
  // the next frame out is that same one rather than 600 KiB of new storage.
  frame.reset();
  CHECK(pool.Acquire().get() == first);
}

TEST_CASE("DepthFramePool never rewrites a frame that is still being read",
          "[nui][depth_frame_pool]") {
  DepthFramePool pool;
  std::vector<std::shared_ptr<DepthFrame>> held;
  for (size_t i = 0; i < DepthFramePool::kSize; ++i) {
    held.push_back(pool.Acquire());
    REQUIRE(held.back() != nullptr);
  }
  for (size_t i = 0; i < held.size(); ++i) {
    for (size_t j = i + 1; j < held.size(); ++j) {
      INFO("pooled frames " << i << " and " << j);
      CHECK(held[i].get() != held[j].get());
    }
  }

  // Every pooled frame is still held, so this one has to be storage of its own
  // rather than one of them handed out twice.
  const auto extra = pool.Acquire();
  REQUIRE(extra != nullptr);
  for (size_t i = 0; i < held.size(); ++i) {
    INFO("pooled frame " << i);
    CHECK(extra.get() != held[i].get());
  }

  // And once a reader lets go, the pool goes back to recycling.
  const DepthFrame* released = held.front().get();
  held.erase(held.begin());
  CHECK(pool.Acquire().get() == released);
}

}  // namespace test
}  // namespace nui
}  // namespace xe
