// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect branch after
// validation. MUST NOT enter upstream PRs or cherry-pick series targeting
// xenia-canary:canary_experimental, or any other upstream contribution.
// .github/CONTRIBUTING.md excludes XDK-derived information, not just copied
// code. See SDK_DERIVED_PRIVATE.md for provenance and integration policy.

#include "xenia/kernel/sdk_private_guest_context.h"

namespace xe {
namespace kernel {
namespace sdk_private {
namespace {

uint32_t ReadBe32(std::span<const uint8_t> bytes, uint32_t offset) {
  return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
         (uint32_t(bytes[offset + 2]) << 8) | uint32_t(bytes[offset + 3]);
}

uint64_t ReadBe64(std::span<const uint8_t> bytes, uint32_t offset) {
  return (uint64_t(ReadBe32(bytes, offset)) << 32) |
         ReadBe32(bytes, offset + 4);
}

void WriteBe32(std::span<uint8_t> bytes, uint32_t offset, uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 24);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 3] = static_cast<uint8_t>(value);
}

void WriteBe64(std::span<uint8_t> bytes, uint32_t offset, uint64_t value) {
  WriteBe32(bytes, offset, static_cast<uint32_t>(value >> 32));
  WriteBe32(bytes, offset + 4, static_cast<uint32_t>(value));
}

}  // namespace

bool DecodeGuestContextRecord(std::span<const uint8_t> bytes,
                              GuestContextRecord& output) {
  if (bytes.size() != kGuestContextRecordSize) {
    return false;
  }

  GuestContextRecord decoded;
  decoded.context_flags = ReadBe32(bytes, 0x000);
  decoded.msr = ReadBe32(bytes, 0x004);
  decoded.iar = ReadBe32(bytes, 0x008);
  decoded.lr = ReadBe32(bytes, 0x00C);
  decoded.ctr = ReadBe64(bytes, 0x010);
  for (uint32_t i = 0; i < decoded.gpr.size(); ++i) {
    decoded.gpr[i] = ReadBe64(bytes, 0x018 + 8 * i);
  }
  decoded.cr = ReadBe32(bytes, 0x118);
  decoded.xer = ReadBe32(bytes, 0x11C);
  decoded.fpscr_bits = ReadBe64(bytes, 0x120);
  for (uint32_t i = 0; i < decoded.fpr_bits.size(); ++i) {
    decoded.fpr_bits[i] = ReadBe64(bytes, 0x128 + 8 * i);
  }
  decoded.user_mode_control = ReadBe32(bytes, 0x228);
  decoded.fill = ReadBe32(bytes, 0x22C);
  for (uint32_t word = 0; word < decoded.vscr.size(); ++word) {
    decoded.vscr[word] = ReadBe32(bytes, 0x230 + 4 * word);
  }
  for (uint32_t reg = 0; reg < decoded.vr.size(); ++reg) {
    for (uint32_t word = 0; word < decoded.vr[reg].size(); ++word) {
      decoded.vr[reg][word] = ReadBe32(bytes, 0x240 + 16 * reg + 4 * word);
    }
  }
  output = decoded;
  return true;
}

bool EncodeGuestContextRecord(const GuestContextRecord& context,
                              std::span<uint8_t> bytes) {
  if (bytes.size() != kGuestContextRecordSize) {
    return false;
  }

  std::array<uint8_t, kGuestContextRecordSize> encoded{};
  WriteBe32(encoded, 0x000, context.context_flags);
  WriteBe32(encoded, 0x004, context.msr);
  WriteBe32(encoded, 0x008, context.iar);
  WriteBe32(encoded, 0x00C, context.lr);
  WriteBe64(encoded, 0x010, context.ctr);
  for (uint32_t i = 0; i < context.gpr.size(); ++i) {
    WriteBe64(encoded, 0x018 + 8 * i, context.gpr[i]);
  }
  WriteBe32(encoded, 0x118, context.cr);
  WriteBe32(encoded, 0x11C, context.xer);
  WriteBe64(encoded, 0x120, context.fpscr_bits);
  for (uint32_t i = 0; i < context.fpr_bits.size(); ++i) {
    WriteBe64(encoded, 0x128 + 8 * i, context.fpr_bits[i]);
  }
  WriteBe32(encoded, 0x228, context.user_mode_control);
  WriteBe32(encoded, 0x22C, context.fill);
  for (uint32_t word = 0; word < context.vscr.size(); ++word) {
    WriteBe32(encoded, 0x230 + 4 * word, context.vscr[word]);
  }
  for (uint32_t reg = 0; reg < context.vr.size(); ++reg) {
    for (uint32_t word = 0; word < context.vr[reg].size(); ++word) {
      WriteBe32(encoded, 0x240 + 16 * reg + 4 * word, context.vr[reg][word]);
    }
  }
  for (uint32_t i = 0; i < encoded.size(); ++i) {
    bytes[i] = encoded[i];
  }
  return true;
}

}  // namespace sdk_private
}  // namespace kernel
}  // namespace xe
