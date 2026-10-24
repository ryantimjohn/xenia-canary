// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect branch after
// validation. MUST NOT enter upstream PRs or cherry-pick series targeting
// xenia-canary:canary_experimental, or any other upstream contribution.
// .github/CONTRIBUTING.md excludes XDK-derived information, not just copied
// code. See SDK_DERIVED_PRIVATE.md for provenance and integration policy.

#ifndef XENIA_KERNEL_SDK_PRIVATE_GUEST_CONTEXT_H_
#define XENIA_KERNEL_SDK_PRIVATE_GUEST_CONTEXT_H_

#include <array>
#include <cstdint>
#include <span>

namespace xe {
namespace kernel {
namespace sdk_private {

inline constexpr uint32_t kGuestContextRecordSize = 0xA40;

// Caller-owned values for a complete wire record, not a host structure overlay.
// Floating-point storage is raw bits. Vector words follow increasing wire
// addresses; no correspondence with host vector lanes is assumed.
struct GuestContextRecord {
  uint32_t context_flags = 0;
  uint32_t msr = 0;
  uint32_t iar = 0;
  uint32_t lr = 0;
  uint64_t ctr = 0;
  std::array<uint64_t, 32> gpr{};
  uint32_t cr = 0;
  uint32_t xer = 0;
  uint64_t fpscr_bits = 0;
  std::array<uint64_t, 32> fpr_bits{};
  uint32_t user_mode_control = 0;
  uint32_t fill = 0;
  std::array<uint32_t, 4> vscr{};
  std::array<std::array<uint32_t, 4>, 128> vr{};

  bool operator==(const GuestContextRecord&) const = default;
};

// Both operations require exactly kGuestContextRecordSize accessible bytes.
// Failure leaves the destination unchanged. Successful operations stage all
// source values before changing the destination, so overlapping source and
// destination storage is supported, including a record's object representation.
// Byte spans may be unaligned. All bits, including unknown context flags and
// Fill, are preserved; flags do not select fields in this full-record codec.
// Flag validity and architectural state precision are the caller's concern.
// These operations do not capture, restore, dispatch, invoke, or map
// PPCContext.
[[nodiscard]] bool DecodeGuestContextRecord(std::span<const uint8_t> bytes,
                                            GuestContextRecord& output);
[[nodiscard]] bool EncodeGuestContextRecord(const GuestContextRecord& context,
                                            std::span<uint8_t> bytes);

}  // namespace sdk_private
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_SDK_PRIVATE_GUEST_CONTEXT_H_
