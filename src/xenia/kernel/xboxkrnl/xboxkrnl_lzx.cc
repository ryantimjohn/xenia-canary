/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/lzx_decompressor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The kernel's block decompression API. A caller creates a context for an LZX
// stream, hands over one compressed block at a time and destroys the context
// when the stream is done; title 4D5308ED's skeletal tracking runtime
// inflates its 22 MB exemplar database this way, and the game's own asset
// loader uses the same calls with a reset between blocks.
//
// The console keeps the decoder state in the memory block the caller passes
// in, since the allocator callbacks are optional and both callers leave them
// null. Xenia decompresses on the host instead and ignores that block, so all
// a context is to the guest is a handle into this table.

namespace {

std::mutex lzx_context_mutex;
std::unordered_map<uint32_t, std::unique_ptr<LzxDecompressor>> lzx_contexts;
// Handles start at one, so that a context is never mistaken for a null
// pointer by a caller that only checks for zero.
uint32_t lzx_next_context = 1;

// How much room a caller is told to leave for one compressed block: LZX can
// grow a block that does not compress. The 256-byte allowance is a host
// fallback, not a measured console result. The asset loader in 4D5308ED bounds
// this answer by 0x9800 for a 0x8000 block (docs/nui.md, "Block
// decompression").
constexpr uint32_t kSourceBufferSlack = 256;

}  // namespace

dword_result_t LDICreateDecompression_entry(
    lpdword_t cb_data_block_max_ptr, lpdword_t configuration_ptr,
    dword_t alloc_callback, dword_t free_callback, lpvoid_t context_memory_ptr,
    lpdword_t cb_src_buffer_min_ptr, lpdword_t context_out_ptr) {
  if (!cb_data_block_max_ptr || !configuration_ptr || !context_out_ptr) {
    XELOGW("LDICreateDecompression: called without its buffers");
    return kLdiErrorConfiguration;
  }

  // The configuration block starts with the LZX window size in bytes.
  const uint32_t block_size_max = cb_data_block_max_ptr.value();
  const uint32_t window_size = configuration_ptr.value();
  XELOGD(
      "LDICreateDecompression(block max {:X}, window {:X}, alloc {:08X}, free "
      "{:08X}, context memory {:08X})",
      block_size_max, window_size, static_cast<uint32_t>(alloc_callback),
      static_cast<uint32_t>(free_callback), context_memory_ptr.guest_address());

  if (!block_size_max || block_size_max > LzxDecompressor::kFrameSize ||
      window_size < LzxDecompressor::kMinWindowSize ||
      window_size > LzxDecompressor::kMaxWindowSize ||
      !xe::is_pow2(window_size)) {
    XELOGW(
        "LDICreateDecompression: block size {:X} or window size {:X} is out of "
        "range",
        block_size_max, window_size);
    return kLdiErrorConfiguration;
  }

  auto decompressor = LzxDecompressor::Create(window_size);
  if (!decompressor) {
    XELOGW("LDICreateDecompression: could not allocate a {:X} byte window",
           window_size);
    return kLdiErrorInsufficientMemory;
  }

  uint32_t context = 0;
  {
    std::lock_guard<std::mutex> lock(lzx_context_mutex);
    context = lzx_next_context++;
    lzx_contexts.emplace(context, std::move(decompressor));
  }

  if (cb_src_buffer_min_ptr) {
    *cb_src_buffer_min_ptr = block_size_max + kSourceBufferSlack;
  }
  *context_out_ptr = context;
  return kLdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

dword_result_t LDIDecompress_entry(dword_t context, lpvoid_t src_ptr,
                                   dword_t src_size, lpvoid_t dst_ptr,
                                   lpdword_t dst_size_ptr) {
  if (!dst_size_ptr) {
    XELOGW("LDIDecompress: called without a size");
    return kLdiErrorFailed;
  }
  // On the way in the size is how much room the destination has, which is the
  // frame's uncompressed size; on the way out it is what was produced.
  const uint32_t dst_capacity = dst_size_ptr.value();

  uint32_t produced = 0;
  int32_t result = kLdiErrorFailed;
  {
    // One lock covers the whole table and the decompression itself: a context
    // belongs to one caller, so there is nothing to gain from finer grain.
    std::lock_guard<std::mutex> lock(lzx_context_mutex);
    auto it = lzx_contexts.find(context);
    if (it == lzx_contexts.end()) {
      XELOGW("LDIDecompress: no context {:08X}",
             static_cast<uint32_t>(context));
      // Every other failure tells the caller that nothing was produced; this
      // one must too, or a title that reads its size back sees the room it
      // asked for reported as bytes it was given.
      *dst_size_ptr = 0;
      return kLdiErrorFailed;
    }
    result =
        it->second->Decompress(src_ptr.as<const uint8_t*>(), src_size,
                               dst_ptr.as<uint8_t*>(), dst_capacity, &produced);
  }

  *dst_size_ptr = produced;
  if (result != kLdiErrorNoError) {
    XELOGW(
        "LDIDecompress: context {:08X} failed with {} on a {} byte block into "
        "{} bytes at {:08X}",
        static_cast<uint32_t>(context), result, static_cast<uint32_t>(src_size),
        dst_capacity, dst_ptr.guest_address());
    return result;
  }
  XELOGD("LDIDecompress(context {:08X}, {} bytes at {:08X}) = {} bytes",
         static_cast<uint32_t>(context), static_cast<uint32_t>(src_size),
         src_ptr.guest_address(), produced);
  return kLdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT2(LDIDecompress, kNone, kImplemented, kHighFrequency);

dword_result_t LDIResetDecompression_entry(dword_t context) {
  XELOGD("LDIResetDecompression(context {:08X})",
         static_cast<uint32_t>(context));
  std::lock_guard<std::mutex> lock(lzx_context_mutex);
  auto it = lzx_contexts.find(context);
  if (it == lzx_contexts.end()) {
    XELOGW("LDIResetDecompression: no context {:08X}",
           static_cast<uint32_t>(context));
    return kLdiErrorFailed;
  }
  const int32_t result = it->second->Reset();
  if (result != kLdiErrorNoError) {
    XELOGW("LDIResetDecompression: context {:08X} failed with {}",
           static_cast<uint32_t>(context), result);
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kImplemented);

dword_result_t LDIDestroyDecompression_entry(dword_t context) {
  XELOGD("LDIDestroyDecompression(context {:08X})",
         static_cast<uint32_t>(context));
  std::lock_guard<std::mutex> lock(lzx_context_mutex);
  if (!lzx_contexts.erase(context)) {
    XELOGW("LDIDestroyDecompression: no context {:08X}",
           static_cast<uint32_t>(context));
    return kLdiErrorFailed;
  }
  return kLdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Lzx);
