// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect branch after
// validation. MUST NOT enter upstream PRs or cherry-pick series targeting
// xenia-canary:canary_experimental, or any other upstream contribution.
// .github/CONTRIBUTING.md excludes XDK-derived information, not just copied
// code. See SDK_DERIVED_PRIVATE.md for provenance and integration policy.

#include "xenia/kernel/sdk_private_guest_context.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace sdk_private {
namespace test {
namespace {

using WireRecord = std::array<uint8_t, kGuestContextRecordSize>;

void PutLiteralBytes(WireRecord& bytes, uint32_t offset,
                     std::initializer_list<uint8_t> values) {
  std::copy(values.begin(), values.end(), bytes.begin() + offset);
}

GuestContextRecord GoldenValues() {
  GuestContextRecord values;
  values.context_flags = 0xA1B2C3D4;
  values.msr = 0x11223344;
  values.iar = 0x55667788;
  values.lr = 0x99AABBCC;
  values.ctr = 0x0123456789ABCDEF;
  values.gpr[0] = 0xFEDCBA9876543210;
  values.gpr[31] = 0x8000000000000001;
  values.cr = 0x10203040;
  values.xer = 0x50607080;
  values.fpscr_bits = 0x7FF8000000000042;
  values.fpr_bits[0] = 0x8000000000000000;
  values.fpr_bits[31] = 0x7FF0000000000001;
  values.user_mode_control = 0x90A0B0C0;
  values.fill = 0xDEADBEEF;
  values.vscr = {0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F};
  values.vr[0] = {0x10213243, 0x54657687, 0x98A9BACB, 0xDCEDFE0F};
  values.vr[127] = {0xFFEEDDCC, 0xBBAA9988, 0x77665544, 0x33221100};
  return values;
}

// Literal positions and bytes are independent of the codec's helpers and any
// host structure layout. Untouched fields must be encoded as zero.
WireRecord GoldenWire() {
  WireRecord bytes{};
  PutLiteralBytes(bytes, 0x000, {0xA1, 0xB2, 0xC3, 0xD4});
  PutLiteralBytes(bytes, 0x004, {0x11, 0x22, 0x33, 0x44});
  PutLiteralBytes(bytes, 0x008, {0x55, 0x66, 0x77, 0x88});
  PutLiteralBytes(bytes, 0x00C, {0x99, 0xAA, 0xBB, 0xCC});
  PutLiteralBytes(bytes, 0x010,
                  {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
  PutLiteralBytes(bytes, 0x018,
                  {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});
  PutLiteralBytes(bytes, 0x110,
                  {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
  PutLiteralBytes(bytes, 0x118, {0x10, 0x20, 0x30, 0x40});
  PutLiteralBytes(bytes, 0x11C, {0x50, 0x60, 0x70, 0x80});
  PutLiteralBytes(bytes, 0x120,
                  {0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42});
  PutLiteralBytes(bytes, 0x128,
                  {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  PutLiteralBytes(bytes, 0x220,
                  {0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
  PutLiteralBytes(bytes, 0x228, {0x90, 0xA0, 0xB0, 0xC0});
  PutLiteralBytes(bytes, 0x22C, {0xDE, 0xAD, 0xBE, 0xEF});
  PutLiteralBytes(bytes, 0x230,
                  {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                   0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F});
  PutLiteralBytes(bytes, 0x240,
                  {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9,
                   0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F});
  PutLiteralBytes(bytes, 0xA30,
                  {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66,
                   0x55, 0x44, 0x33, 0x22, 0x11, 0x00});
  return bytes;
}

}  // namespace

TEST_CASE("Private context encoding matches literal wire positions",
          "[kernel][sdk-private][context]") {
  WireRecord encoded;
  encoded.fill(0xCD);
  REQUIRE(EncodeGuestContextRecord(GoldenValues(), encoded));
  CHECK(encoded == GoldenWire());
}

TEST_CASE("Private context decoding preserves raw scalar and vector bits",
          "[kernel][sdk-private][context]") {
  GuestContextRecord decoded;
  REQUIRE(DecodeGuestContextRecord(GoldenWire(), decoded));
  CHECK(decoded == GoldenValues());
  CHECK(decoded.context_flags == 0xA1B2C3D4);
  CHECK(decoded.fill == 0xDEADBEEF);
  CHECK(decoded.fpscr_bits == 0x7FF8000000000042);
  CHECK(decoded.fpr_bits[0] == 0x8000000000000000);
  CHECK(decoded.fpr_bits[31] == 0x7FF0000000000001);
}

TEST_CASE("Private context carries all 128 four-word vector registers",
          "[kernel][sdk-private][context]") {
  GuestContextRecord values;
  WireRecord expected{};
  // Each word has a literal byte marker, register number, marker, word number.
  // Compare whole records, avoiding hundreds of individual mirror assertions.
  for (uint32_t reg = 0; reg < 128; ++reg) {
    for (uint32_t word = 0; word < 4; ++word) {
      values.vr[reg][word] = 0xA0001000 | (reg << 16) | word;
      PutLiteralBytes(
          expected, 0x240 + reg * 16 + word * 4,
          {0xA0, static_cast<uint8_t>(reg), 0x10, static_cast<uint8_t>(word)});
    }
  }
  WireRecord encoded{};
  REQUIRE(EncodeGuestContextRecord(values, encoded));
  CHECK(encoded == expected);
  GuestContextRecord decoded;
  REQUIRE(DecodeGuestContextRecord(expected, decoded));
  CHECK(decoded == values);
}

TEST_CASE("Private context round trips arbitrary bytes without flag admission",
          "[kernel][sdk-private][context]") {
  WireRecord original;
  for (uint32_t i = 0; i < original.size(); ++i) {
    original[i] = static_cast<uint8_t>((i * 157 + 31) ^ (i >> 8));
  }
  GuestContextRecord decoded;
  REQUIRE(DecodeGuestContextRecord(original, decoded));
  WireRecord encoded{};
  REQUIRE(EncodeGuestContextRecord(decoded, encoded));
  CHECK(encoded == original);
}

TEST_CASE("Private context rejects nonexact sizes without changing outputs",
          "[kernel][sdk-private][context]") {
  std::array<uint8_t, kGuestContextRecordSize + 3> storage;
  storage.fill(0xCD);
  const auto before = storage;
  GuestContextRecord output = GoldenValues();
  const auto output_before = output;
  for (uint32_t size :
       {0U, kGuestContextRecordSize - 1, kGuestContextRecordSize + 1}) {
    INFO("Rejected span size: " << size);
    auto bytes = std::span<uint8_t>(storage).subspan(1, size);
    CHECK_FALSE(DecodeGuestContextRecord(bytes, output));
    CHECK(output == output_before);
    CHECK_FALSE(EncodeGuestContextRecord(output_before, bytes));
    CHECK(storage == before);
  }
}

TEST_CASE("Private context accepts unaligned spans and preserves canaries",
          "[kernel][sdk-private][context]") {
  alignas(16) std::array<uint8_t, kGuestContextRecordSize + 2> storage;
  storage.fill(0xCA);
  auto bytes = std::span<uint8_t>(storage).subspan(1, kGuestContextRecordSize);
  REQUIRE(EncodeGuestContextRecord(GoldenValues(), bytes));
  const auto golden = GoldenWire();
  CHECK(std::equal(bytes.begin(), bytes.end(), golden.begin()));
  CHECK(storage.front() == 0xCA);
  CHECK(storage.back() == 0xCA);

  struct GuardedRecord {
    std::array<uint8_t, 16> before;
    GuestContextRecord output;
    std::array<uint8_t, 16> after;
  } guarded;
  guarded.before.fill(0xAC);
  guarded.after.fill(0xAC);
  REQUIRE(DecodeGuestContextRecord(bytes, guarded.output));
  CHECK(guarded.output == GoldenValues());
  const std::array<uint8_t, 16> canary = {0xAC, 0xAC, 0xAC, 0xAC, 0xAC, 0xAC,
                                          0xAC, 0xAC, 0xAC, 0xAC, 0xAC, 0xAC,
                                          0xAC, 0xAC, 0xAC, 0xAC};
  CHECK(guarded.before == canary);
  CHECK(guarded.after == canary);
  CHECK(storage.front() == 0xCA);
  CHECK(storage.back() == 0xCA);
}

TEST_CASE("Private context stages overlapping record and byte storage",
          "[kernel][sdk-private][context]") {
  GuestContextRecord record = GoldenValues();
  // Character access to a live object's representation is permitted. The codec
  // must read the whole source before overwriting these overlapping bytes.
  auto representation =
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(&record), sizeof(record));
  REQUIRE(representation.size() >= kGuestContextRecordSize);
  auto bytes = representation.first(kGuestContextRecordSize);
  const auto golden = GoldenWire();
  REQUIRE(EncodeGuestContextRecord(record, bytes));
  CHECK(std::equal(bytes.begin(), bytes.end(), golden.begin()));

  // Reestablish the wire bytes explicitly so this also tests decode
  // independently of successful encoding into an overlapping object
  // representation.
  std::copy(golden.begin(), golden.end(), bytes.begin());
  REQUIRE(DecodeGuestContextRecord(bytes, record));
  CHECK(record == GoldenValues());
}

}  // namespace test
}  // namespace sdk_private
}  // namespace kernel
}  // namespace xe
