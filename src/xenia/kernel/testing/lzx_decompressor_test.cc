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
#include <cstdint>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#include "third_party/catch/include/catch.hpp"

DECLARE_int32(log_level);

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace test {

namespace {

// Writes the LZX bitstream of one or more uncompressed blocks, exactly as
// third_party/mspack/lzxd.c decodes them: bits are taken most significant
// first out of 16-bit little-endian words.
class LzxStreamWriter {
 public:
  void WriteBits(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      bits_.push_back((value >> i) & 1);
    }
  }

  // Pads the pending bits with zeroes up to the next 16-bit word and emits
  // them. This is the realignment an uncompressed block performs before its
  // stored R0/R1/R2 words.
  void AlignToWord() {
    while (bits_.size() % 16) {
      bits_.push_back(0);
    }
    for (size_t i = 0; i < bits_.size(); i += 16) {
      uint32_t word = 0;
      for (size_t j = 0; j < 16; ++j) {
        word = (word << 1) | bits_[i + j];
      }
      bytes_.push_back(static_cast<uint8_t>(word & 0xFF));
      bytes_.push_back(static_cast<uint8_t>(word >> 8));
    }
    bits_.clear();
  }

  void WriteUint32LE(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
  }

  void WriteBytes(const std::vector<uint8_t>& data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<bool> bits_;
  std::vector<uint8_t> bytes_;
};

// Appends one LZX uncompressed block holding data. The very first block of a
// stream carries the Intel E8 header bit in front of the block header.
void AppendUncompressedBlock(LzxStreamWriter* writer, bool first_block,
                             const std::vector<uint8_t>& data) {
  if (first_block) {
    writer->WriteBits(0, 1);  // No Intel E8 transform.
  }
  writer->WriteBits(3, 3);  // LZX_BLOCKTYPE_UNCOMPRESSED.
  // The block length is 24 bits, read as 16 bits and then 8.
  const uint32_t length = static_cast<uint32_t>(data.size());
  writer->WriteBits(length >> 8, 16);
  writer->WriteBits(length & 0xFF, 8);
  writer->AlignToWord();
  // The block restores the LRU match offsets before its payload.
  writer->WriteUint32LE(1);
  writer->WriteUint32LE(1);
  writer->WriteUint32LE(1);
  writer->WriteBytes(data);
}

// mspack reports a stream it cannot decode through xenia's log, which is
// exactly what a test that feeds it a broken stream wants to keep quiet.
class SilenceDecoderWarnings {
 public:
  SilenceDecoderWarnings() : saved_log_level_(cvars::log_level) {
    cvars::log_level = static_cast<int32_t>(LogLevel::Error);
  }
  ~SilenceDecoderWarnings() { cvars::log_level = saved_log_level_; }

 private:
  int32_t saved_log_level_;
};

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(i * 7 + seed);
  }
  return data;
}

// The window 4D5308ED' skeletal tracking database asks for.
constexpr uint32_t kWindowSize = 0x80000;

}  // namespace

TEST_CASE("LzxDecompressor decodes a single short frame", "[lzx]") {
  const std::vector<uint8_t> payload = MakePattern(100, 0x11);
  LzxStreamWriter writer;
  AppendUncompressedBlock(&writer, true, payload);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  std::vector<uint8_t> out(payload.size());
  uint32_t out_size = 0;
  REQUIRE(decompressor->Decompress(writer.bytes().data(),
                                   static_cast<uint32_t>(writer.bytes().size()),
                                   out.data(),
                                   static_cast<uint32_t>(out.size()),
                                   &out_size) == kLdiErrorNoError);
  CHECK(out_size == payload.size());
  CHECK(out == payload);
}

TEST_CASE("LzxDecompressor decodes two frames from two blocks", "[lzx]") {
  // A full frame followed by a short final one, the way the skeletal tracking
  // database is laid out: one block per frame, handed over one call at a time.
  const std::vector<uint8_t> first = MakePattern(32768, 0x23);
  const std::vector<uint8_t> second = MakePattern(100, 0x55);
  LzxStreamWriter first_writer;
  AppendUncompressedBlock(&first_writer, true, first);
  LzxStreamWriter second_writer;
  AppendUncompressedBlock(&second_writer, false, second);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  std::vector<uint8_t> out(first.size());
  uint32_t out_size = 0;
  REQUIRE(decompressor->Decompress(
              first_writer.bytes().data(),
              static_cast<uint32_t>(first_writer.bytes().size()), out.data(),
              static_cast<uint32_t>(out.size()),
              &out_size) == kLdiErrorNoError);
  CHECK(out_size == first.size());
  CHECK(out == first);

  out.assign(second.size(), 0);
  REQUIRE(decompressor->Decompress(
              second_writer.bytes().data(),
              static_cast<uint32_t>(second_writer.bytes().size()), out.data(),
              static_cast<uint32_t>(out.size()),
              &out_size) == kLdiErrorNoError);
  CHECK(out_size == second.size());
  CHECK(out == second);
}

TEST_CASE("LzxDecompressor starts over after a reset", "[lzx]") {
  const std::vector<uint8_t> payload = MakePattern(100, 0x11);
  LzxStreamWriter writer;
  AppendUncompressedBlock(&writer, true, payload);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  std::vector<uint8_t> out(payload.size());
  uint32_t out_size = 0;
  REQUIRE(decompressor->Decompress(writer.bytes().data(),
                                   static_cast<uint32_t>(writer.bytes().size()),
                                   out.data(),
                                   static_cast<uint32_t>(out.size()),
                                   &out_size) == kLdiErrorNoError);
  CHECK(out == payload);

  // After the reset the same stream decodes again from its first byte, header
  // bit and all, which would not parse as a continuation of the first one.
  REQUIRE(decompressor->Reset() == kLdiErrorNoError);
  out.assign(payload.size(), 0);
  out_size = 0;
  REQUIRE(decompressor->Decompress(writer.bytes().data(),
                                   static_cast<uint32_t>(writer.bytes().size()),
                                   out.data(),
                                   static_cast<uint32_t>(out.size()),
                                   &out_size) == kLdiErrorNoError);
  CHECK(out_size == payload.size());
  CHECK(out == payload);
}

TEST_CASE("LzxDecompressor fails a truncated block", "[lzx]") {
  const SilenceDecoderWarnings silence;
  const std::vector<uint8_t> payload = MakePattern(100, 0x11);
  LzxStreamWriter writer;
  AppendUncompressedBlock(&writer, true, payload);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  std::vector<uint8_t> out(payload.size());
  uint32_t out_size = 123;
  CHECK(decompressor->Decompress(writer.bytes().data(), 20, out.data(),
                                 static_cast<uint32_t>(out.size()),
                                 &out_size) == kLdiErrorFailed);
  CHECK(out_size == 0);
}

TEST_CASE("LzxDecompressor fails a destination with no room", "[lzx]") {
  const std::vector<uint8_t> payload = MakePattern(100, 0x11);
  LzxStreamWriter writer;
  AppendUncompressedBlock(&writer, true, payload);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  uint8_t out = 0;
  uint32_t out_size = 123;
  CHECK(decompressor->Decompress(
            writer.bytes().data(), static_cast<uint32_t>(writer.bytes().size()),
            &out, 0, &out_size) == kLdiErrorBufferOverflow);
  CHECK(out_size == 0);
}

TEST_CASE("LzxDecompressor takes a short destination as a short frame",
          "[lzx]") {
  // A destination with some room but not enough for the block's frame. The
  // capacity is the frame's uncompressed size as the caller knows it, and
  // nothing in the stream tells the decoder otherwise, so a caller that asks
  // for fewer bytes than the block holds is asking for a shorter final frame
  // and gets exactly that many bytes.
  const std::vector<uint8_t> payload = MakePattern(100, 0x11);
  LzxStreamWriter writer;
  AppendUncompressedBlock(&writer, true, payload);

  auto decompressor = LzxDecompressor::Create(kWindowSize);
  REQUIRE(decompressor != nullptr);

  std::vector<uint8_t> out(50);
  uint32_t out_size = 123;
  CHECK(decompressor->Decompress(writer.bytes().data(),
                                 static_cast<uint32_t>(writer.bytes().size()),
                                 out.data(), static_cast<uint32_t>(out.size()),
                                 &out_size) == kLdiErrorNoError);
  CHECK(out_size == out.size());
  CHECK(std::equal(out.begin(), out.end(), payload.begin()));
}

TEST_CASE("LzxDecompressor refuses a window outside the LZX range", "[lzx]") {
  CHECK(LzxDecompressor::Create(0x1000) == nullptr);
  CHECK(LzxDecompressor::Create(0x400000) == nullptr);
  // A window size has to be a power of two as well.
  CHECK(LzxDecompressor::Create(0x30000) == nullptr);
  CHECK(LzxDecompressor::Create(LzxDecompressor::kMinWindowSize) != nullptr);
  CHECK(LzxDecompressor::Create(LzxDecompressor::kMaxWindowSize) != nullptr);
}

}  // namespace test
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
