/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_LZX_DECOMPRESSOR_H_
#define XENIA_KERNEL_XBOXKRNL_LZX_DECOMPRESSOR_H_

#include <cstdint>
#include <memory>

struct lzxd_stream;
struct mspack_file;
struct mspack_system;

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Result codes of the kernel's block decompression API (the LDI exports), in
// the guest's numbering.
//
// The decoder underneath has a vocabulary of its own - a bad stream, a short
// read, a failed write, a sticky error left on the stream by an earlier call -
// and every one of those becomes kLdiErrorFailed, because that is what the
// exports give a title for a block they could not decode and because mspack's
// distinctions say nothing a title could act on. The other three codes are
// only ever raised before the decoder is reached: the window or the stream
// could not be allocated, the destination has no room, or the shim refused
// the configuration the title asked for.
enum LdiError : int32_t {
  kLdiErrorNoError = 0,
  kLdiErrorFailed = 1,
  kLdiErrorConfiguration = 2,
  kLdiErrorBufferOverflow = 3,
  kLdiErrorInsufficientMemory = 4,
};

// One LZX decompression context, driven one compressed block at a time the
// way the LDI exports present it to a title.
//
// The compressed stream is a sequence of blocks, each holding exactly one LZX
// frame: 32768 bytes of output, or fewer for the last frame of a stream. Each
// call hands over one block's bytes and asks for that frame's uncompressed
// size; the decoder's window and Huffman trees carry over between calls, so
// the blocks of a stream have to arrive in order and none may be skipped.
// Reset starts a new stream.
//
// This class takes and produces host buffers only, so the guest side of the
// exports stays in the shims.
class LzxDecompressor {
 public:
  // Uncompressed size of a full LZX frame. A block never produces more.
  static constexpr uint32_t kFrameSize = 32768;
  // The decoding window sizes LZX allows, in bytes.
  static constexpr uint32_t kMinWindowSize = 32 * 1024;
  static constexpr uint32_t kMaxWindowSize = 2 * 1024 * 1024;

  // Creates a decompressor with the given window size, which has to be a
  // power of two between kMinWindowSize and kMaxWindowSize. Returns null for
  // any other size and when the window cannot be allocated.
  static std::unique_ptr<LzxDecompressor> Create(uint32_t window_size);

  ~LzxDecompressor();

  LzxDecompressor(const LzxDecompressor&) = delete;
  LzxDecompressor& operator=(const LzxDecompressor&) = delete;

  // Decompresses the single LZX frame in the block at src into dst, which has
  // to have room for dst_capacity bytes. dst_capacity is the frame's
  // uncompressed size as the caller knows it; anything above kFrameSize is
  // read as a full frame. Writes the number of bytes produced to out_size,
  // which is zero unless the result is kLdiErrorNoError.
  int32_t Decompress(const uint8_t* src, uint32_t src_size, uint8_t* dst,
                     uint32_t dst_capacity, uint32_t* out_size);

  // Throws the window, the bit reader and the frame counter away so that the
  // next Decompress call starts a new LZX stream.
  int32_t Reset();

 private:
  LzxDecompressor() = default;

  // The buffers of the call in flight, which the mspack callbacks read from
  // and write to.
  struct CallBuffers {
    const uint8_t* src = nullptr;
    uint32_t src_size = 0;
    uint32_t src_offset = 0;
    uint8_t* dst = nullptr;
    uint32_t dst_capacity = 0;
    uint32_t dst_size = 0;
  };

  static int Read(mspack_file* file, void* buffer, int count);
  static int Write(mspack_file* file, void* buffer, int count);

  // Creates the mspack stream, replacing any existing one.
  bool CreateStream();

  int window_bits_ = 0;
  CallBuffers buffers_;
  std::unique_ptr<mspack_system> system_;
  lzxd_stream* stream_ = nullptr;
};

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_LZX_DECOMPRESSOR_H_
