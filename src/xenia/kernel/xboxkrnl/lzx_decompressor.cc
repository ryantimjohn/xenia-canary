/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/lzx_decompressor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "xenia/base/math.h"

#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

// One block is read in one go, so the buffer only has to outsize the largest
// block a caller may hand over.
constexpr int kInputBufferSize = 0x10000;

void* Allocate(mspack_system* self, size_t size) {
  return std::calloc(size, 1);
}

void Free(void* ptr) { std::free(ptr); }

void Copy(void* src, void* dst, size_t size) { std::memcpy(dst, src, size); }

}  // namespace

std::unique_ptr<LzxDecompressor> LzxDecompressor::Create(uint32_t window_size) {
  uint32_t window_bits = 0;
  if (window_size < kMinWindowSize || window_size > kMaxWindowSize ||
      !xe::is_pow2(window_size) ||
      !xe::bit_scan_forward(window_size, &window_bits)) {
    return nullptr;
  }

  auto decompressor = std::unique_ptr<LzxDecompressor>(new LzxDecompressor());
  decompressor->window_bits_ = static_cast<int>(window_bits);
  if (!decompressor->CreateStream()) {
    return nullptr;
  }
  return decompressor;
}

LzxDecompressor::~LzxDecompressor() {
  if (stream_) {
    lzxd_free(stream_);
  }
}

bool LzxDecompressor::CreateStream() {
  if (stream_) {
    lzxd_free(stream_);
    stream_ = nullptr;
  }
  if (!system_) {
    system_ = std::make_unique<mspack_system>();
    std::memset(system_.get(), 0, sizeof(*system_));
    system_->read = &LzxDecompressor::Read;
    system_->write = &LzxDecompressor::Write;
    system_->alloc = &Allocate;
    system_->free = &Free;
    system_->copy = &Copy;
  }
  // The stream never resets itself: a reset interval of zero is how CAB-style
  // LZX streams, and the ones the LDI exports carry, are framed. The output
  // length is set per block, once the caller says how long the frame is.
  auto* handle = reinterpret_cast<mspack_file*>(&buffers_);
  stream_ = lzxd_init(system_.get(), handle, handle, window_bits_, 0,
                      kInputBufferSize, 0, 0);
  return stream_ != nullptr;
}

int LzxDecompressor::Read(mspack_file* file, void* buffer, int count) {
  // Returning less than asked for, and nothing at all once the block is used
  // up, is how mspack is told where the input ends.
  auto* buffers = reinterpret_cast<CallBuffers*>(file);
  if (count <= 0) {
    return 0;
  }
  const uint32_t served = std::min(static_cast<uint32_t>(count),
                                   buffers->src_size - buffers->src_offset);
  if (served) {
    std::memcpy(buffer, buffers->src + buffers->src_offset, served);
    buffers->src_offset += served;
  }
  return static_cast<int>(served);
}

int LzxDecompressor::Write(mspack_file* file, void* buffer, int count) {
  auto* buffers = reinterpret_cast<CallBuffers*>(file);
  if (count <= 0) {
    return count;
  }
  const uint32_t wanted = static_cast<uint32_t>(count);
  if (wanted > buffers->dst_capacity - buffers->dst_size) {
    // Never reached while the caller's frame size is respected, but a short
    // write is how mspack is told the output does not fit.
    return 0;
  }
  std::memcpy(buffers->dst + buffers->dst_size, buffer, wanted);
  buffers->dst_size += wanted;
  return count;
}

int32_t LzxDecompressor::Decompress(const uint8_t* src, uint32_t src_size,
                                    uint8_t* dst, uint32_t dst_capacity,
                                    uint32_t* out_size) {
  *out_size = 0;
  if (!stream_) {
    return kLdiErrorInsufficientMemory;
  }
  if (!dst || !dst_capacity) {
    return kLdiErrorBufferOverflow;
  }
  if (!src || !src_size) {
    return kLdiErrorFailed;
  }

  // A block holds one frame, so anything the caller asks for above a frame is
  // really a request for the whole frame.
  const uint32_t expected = std::min(dst_capacity, kFrameSize);

  buffers_.src = src;
  buffers_.src_size = src_size;
  buffers_.src_offset = 0;
  buffers_.dst = dst;
  buffers_.dst_capacity = dst_capacity;
  buffers_.dst_size = 0;

  // Each block starts on a fresh bit reader: whatever the decoder has read
  // beyond the previous frame is that frame's padding, and the bytes of the
  // next frame are in the block that arrives now.
  stream_->i_ptr = stream_->inbuf;
  stream_->i_end = stream_->inbuf;
  stream_->bit_buffer = 0;
  stream_->bits_left = 0;
  stream_->input_end = 0;
  // Telling the decoder where the output ends is what makes a final frame
  // shorter than 32768 bytes decode as a final frame.
  lzxd_set_output_length(stream_, stream_->offset + expected);

  // mspack decodes one frame more than asked for when the request ends
  // exactly on a frame boundary, keeping the extra frame for the next call.
  // That frame's bytes are in the next block, which is not here yet, so the
  // request stops one byte short and a second call collects the byte the
  // first one has already decoded.
  const bool ends_on_frame_boundary = expected == kFrameSize;
  int error = lzxd_decompress(stream_,
                              ends_on_frame_boundary ? expected - 1 : expected);
  if (!error && ends_on_frame_boundary) {
    error = lzxd_decompress(stream_, 1);
  }

  const uint32_t produced = buffers_.dst_size;
  buffers_ = CallBuffers();
  if (error != MSPACK_ERR_OK) {
    return kLdiErrorFailed;
  }
  *out_size = produced;
  return kLdiErrorNoError;
}

int32_t LzxDecompressor::Reset() {
  return CreateStream() ? kLdiErrorNoError : kLdiErrorInsufficientMemory;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
