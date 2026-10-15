// FORK-ONLY INTEGRATION STAGING: Eligible for this fork's kinect after
// validation. Keep SDK-derived work out of upstream PR/cherry-pick series,
// including xenia-canary:canary_experimental. CONTRIBUTING.md excludes
// XDK-derived information, not just copied code.

#include "xenia/cpu/ppc/ppc_cpp_first_phase.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace xe::cpu::ppc {
namespace {

using Status = CppFirstPhaseAdmissionStatus;
constexpr uint64_t kGuestEnd = uint64_t{1} << 32;
constexpr uint64_t kReadLimit = 224;

uint32_t ReadBe32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

CppFirstPhaseAdmission Failure(Status status) { return {status, 0, 0}; }

class Reader {
 public:
  explicit Reader(const ExceptionMetadataInput& image)
      : image_(image),
        remaining_(std::min(image.limits.max_read_bytes, kReadLimit)) {}

  template <size_t N>
  bool Words(uint32_t address, bool executable,
             std::array<uint32_t, N>& words) {
    std::array<uint8_t, N * 4> bytes{};
    if (!Read(address, executable, bytes)) {
      return false;
    }
    for (size_t i = 0; i < N; ++i) {
      words[i] = ReadBe32(bytes.data() + i * 4);
    }
    return true;
  }

  Status status() const { return status_; }

 private:
  bool Read(uint32_t address, bool executable, std::span<uint8_t> bytes) {
    if (bytes.size() > remaining_ || reads_ == 9) {
      status_ = Status::kResourceLimit;
      return false;
    }
    const uint64_t end = uint64_t(address) + bytes.size();
    if ((address & 3) || image_.image_base >= image_.image_end ||
        image_.image_end > kGuestEnd || address < image_.image_base ||
        end > image_.image_end || !image_.read) {
      status_ = Status::kBounds;
      return false;
    }
    const bool allowed =
        std::any_of(image_.sections.begin(), image_.sections.end(),
                    [&](const ExceptionMetadataSection& section) {
                      return section.available && section.readable &&
                             (!executable || section.executable) &&
                             section.begin >= image_.image_base &&
                             section.end <= image_.image_end &&
                             section.begin <= address && end <= section.end;
                    });
    if (!allowed) {
      status_ = Status::kBounds;
      return false;
    }
    remaining_ -= bytes.size();
    ++reads_;
    try {
      if (image_.read(address, bytes)) {
        return true;
      }
    } catch (...) {
      // A live-memory adapter must contain host failures; no usable admission
      // may escape a failed or throwing exact read.
    }
    status_ = Status::kReadFailed;
    return false;
  }

  const ExceptionMetadataInput& image_;
  uint64_t remaining_;
  size_t reads_ = 0;
  Status status_ = Status::kUnsupported;
};

bool RelativeTarget(uint32_t pc, uint32_t instruction, bool linked,
                    uint32_t& target) {
  if ((instruction & 0xFC000003) != (linked ? 0x48000001 : 0x48000000)) {
    return false;
  }
  int64_t displacement = instruction & 0x03FFFFFC;
  if (displacement & 0x02000000) {
    displacement -= 0x04000000;
  }
  const int64_t address = int64_t(pc) + displacement;
  if (address < 0 || uint64_t(address) >= kGuestEnd || (address & 3)) {
    return false;
  }
  target = uint32_t(address);
  return true;
}

bool ConditionalTarget(uint32_t pc, uint32_t instruction, uint32_t form,
                       uint32_t& target) {
  if ((instruction & 0xFFFF0003) != form) {
    return false;
  }
  int64_t displacement = instruction & 0xFFFC;
  if (displacement & 0x8000) {
    displacement -= 0x10000;
  }
  const int64_t address = int64_t(pc) + displacement;
  if (address < 0 || uint64_t(address) >= kGuestEnd || (address & 3)) {
    return false;
  }
  target = uint32_t(address);
  return true;
}

// Supplied-game raw instruction evidence, including both actually executed
// SP-relative save/restore bundles. Zero placeholders are decoded branches.
constexpr std::array<uint32_t, 14> kWrapper = {
    0x81660004, 0x816B0000, 0x80EBFFFC, 0x8167000C, 0x2B0B0000,
    0x419A0014, 0x81640004, 0x2B0B0000, 0x419A0008, 0x808B0004,
    0x39400000, 0x39200000, 0x39000000, 0};
constexpr std::array<uint32_t, 13> kCore = {
    0x7D8802A6, 0,          0x3BE1FF70, 0x9421FF70, 0x7C9B2378,
    0x7CBC2B78, 0x7CDE3378, 0x7CFD3B78, 0x7D4B5378, 0x81430004,
    0x71470066, 0x28070000, 0};
constexpr std::array<uint32_t, 7> kSave = {0xFB61FFD0, 0xFB81FFD8, 0xFBA1FFE0,
                                           0xFBC1FFE8, 0xFBE1FFF0, 0x9181FFF8,
                                           0x4E800020};
constexpr std::array<uint32_t, 8> kRestore = {
    0xEB61FFD0, 0xEB81FFD8, 0xEBA1FFE0, 0xEBC1FFE8,
    0xEBE1FFF0, 0x8181FFF8, 0x7D8803A6, 0x4E800020};

}  // namespace

CppFirstPhaseAdmission AdmitNoCatchCppFirstPhase(
    const ExceptionMetadataFunction& function, uint32_t exception_flags,
    const ExceptionMetadataInput& owned_image,
    UnwindCaptureProvenance provenance) noexcept {
  if (provenance != UnwindCaptureProvenance::kCoherentPrecise) {
    return Failure(Status::kUntrustedEvidence);
  }
  if (owned_image.sections.size() > 256) {
    return Failure(Status::kResourceLimit);
  }
  const uint32_t count = (function.raw_info >> 8) & 0x3FFFFF;
  if ((exception_flags & 0x66) != 0 ||
      function.status != ExceptionMetadataStatus::kValid ||
      !function.packed_candidate ||
      (function.raw_info & 0xC0000000) != 0xC0000000 || function.begin < 8 ||
      !function.table_address || !count ||
      function.length_instructions != count ||
      function.prologue_instructions != (function.raw_info & 0xFF) ||
      function.prologue_instructions > count ||
      function.end != uint64_t(function.begin) + uint64_t(count) * 4 ||
      function.end > owned_image.image_end || !function.handler ||
      !function.handler->address || !function.handler->data_address ||
      function.handler->imported_c_specific ||
      (function.handler->status != ExceptionMetadataStatus::kOpaque &&
       function.handler->status != ExceptionMetadataStatus::kValid)) {
    return Failure(Status::kUnsupported);
  }

  Reader reader(owned_image);
  std::array<uint32_t, 2> entry{}, prefix{};
  std::array<uint32_t, 4> func_info{};
  if (!reader.Words(function.table_address, false, entry) ||
      !reader.Words(function.begin - 8, false, prefix) ||
      !reader.Words(function.handler->data_address, false, func_info)) {
    return Failure(reader.status());
  }
  if (entry[0] != function.begin || entry[1] != function.raw_info ||
      prefix[0] != function.handler->address ||
      prefix[1] != function.handler->data_address ||
      func_info[0] != 0x19930522 || func_info[3] != 0) {
    return Failure(Status::kUnsupported);
  }

  std::array<uint32_t, 14> wrapper{};
  const uint32_t handler = function.handler->address;
  if (!reader.Words(handler, true, wrapper)) {
    return Failure(reader.status());
  }
  for (size_t i = 0; i < kWrapper.size() - 1; ++i) {
    if (wrapper[i] != kWrapper[i]) {
      return Failure(Status::kUnsupported);
    }
  }
  uint32_t core_address = 0;
  if (!RelativeTarget(handler + 0x34, wrapper[13], false, core_address)) {
    return Failure(Status::kUnsupported);
  }
  std::array<uint32_t, 13> core{};
  if (!reader.Words(core_address, true, core)) {
    return Failure(reader.status());
  }
  for (size_t i = 0; i < kCore.size(); ++i) {
    if (i != 1 && i != 12 && core[i] != kCore[i]) {
      return Failure(Status::kUnsupported);
    }
  }
  uint32_t save_address = 0, zero_try_address = 0;
  if (!RelativeTarget(core_address + 4, core[1], true, save_address) ||
      !ConditionalTarget(core_address + 0x30, core[12], 0x41820000,
                         zero_try_address)) {
    return Failure(Status::kUnsupported);
  }
  std::array<uint32_t, 3> zero_try{};
  if (!reader.Words(zero_try_address, true, zero_try)) {
    return Failure(reader.status());
  }
  uint32_t return_address = 0;
  if (zero_try[0] != 0x815D000C || zero_try[1] != 0x2B0A0000 ||
      !ConditionalTarget(zero_try_address + 8, zero_try[2], 0x419A0000,
                         return_address)) {
    return Failure(Status::kUnsupported);
  }
  std::array<uint32_t, 3> return_one{};
  if (!reader.Words(return_address, true, return_one)) {
    return Failure(reader.status());
  }
  uint32_t restore_address = 0;
  if (return_one[0] != 0x38600001 || return_one[1] != 0x383F0090 ||
      !RelativeTarget(return_address + 8, return_one[2], false,
                      restore_address)) {
    return Failure(Status::kUnsupported);
  }
  std::array<uint32_t, 7> save{};
  std::array<uint32_t, 8> restore{};
  if (!reader.Words(save_address, true, save) ||
      !reader.Words(restore_address, true, restore)) {
    return Failure(reader.status());
  }
  if (save != kSave || restore != kRestore) {
    return Failure(Status::kUnsupported);
  }
  return {Status::kAdmitted, handler, function.table_address};
}

}  // namespace xe::cpu::ppc
