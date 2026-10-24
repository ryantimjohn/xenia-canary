// FORK-ONLY INTEGRATION STAGING: Eligible for this fork's kinect after
// validation. Keep SDK-derived work out of upstream PR/cherry-pick series,
// including xenia-canary:canary_experimental. CONTRIBUTING.md excludes
// XDK-derived information, not just copied code.

#include "xenia/cpu/ppc/ppc_cpp_first_phase.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {
namespace {

using Status = CppFirstPhaseAdmissionStatus;
constexpr auto kOwned = UnwindCaptureProvenance::kCoherentPrecise;

// These are supplied-game words, not a host CONTEXT or an SDK implementation.
// All addresses and data below are synthetic owned evidence. Relocations
// include backward branch targets; no actual runtime capture/invocation is
// claimed.
struct Fixture {
  uint32_t base;
  uint32_t wrapper, core, zero_try, return_one, save, restore;
  ExceptionMetadataFunction function;
  std::array<ExceptionMetadataSection, 2> sections;
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x4000);
  size_t read_calls = 0;
  size_t read_bytes = 0;
  size_t fail_read = 0;
  bool throw_read = false;

  explicit Fixture(uint32_t image_base = 0x10000000, bool backwards = false)
      : base(image_base) {
    wrapper = base + (backwards ? 0x1800 : 0x200);
    core = base + (backwards ? 0x1000 : 0x400);
    zero_try = base + (backwards ? 0xF00 : 0x600);
    return_one = base + (backwards ? 0xE00 : 0x700);
    save = base + (backwards ? 0xD00 : 0x800);
    restore = base + (backwards ? 0xC00 : 0x900);
    sections[0].begin = base;
    sections[0].end = uint64_t(base) + 0x2000;
    sections[0].readable = true;
    sections[0].executable = true;
    sections[1].begin = base + 0x2000;
    sections[1].end = uint64_t(base) + 0x4000;
    sections[1].readable = true;
    function.begin = base + 0x1C00;
    function.table_address = base + 0x2100;
    function.raw_info = 0xC0001305;
    function.packed_candidate = true;
    function.prologue_instructions = 5;
    function.length_instructions = 19;
    function.end = uint64_t(function.begin) + 19 * 4;
    function.status = ExceptionMetadataStatus::kValid;
    function.handler.emplace();
    function.handler->address = wrapper;
    function.handler->data_address = base + 0x2200;
    function.handler->status = ExceptionMetadataStatus::kOpaque;
    Put(function.table_address, {function.begin, function.raw_info});
    Put(function.begin - 8, {wrapper, function.handler->data_address});
    Put(function.handler->data_address, {0x19930522, 1, base + 0x2300, 0});
    Put(wrapper,
        {0x81660004, 0x816B0000, 0x80EBFFFC, 0x8167000C, 0x2B0B0000, 0x419A0014,
         0x81640004, 0x2B0B0000, 0x419A0008, 0x808B0004, 0x39400000, 0x39200000,
         0x39000000, Branch(wrapper + 0x34, core, false)});
    Put(core, {0x7D8802A6, Branch(core + 4, save, true), 0x3BE1FF70, 0x9421FF70,
               0x7C9B2378, 0x7CBC2B78, 0x7CDE3378, 0x7CFD3B78, 0x7D4B5378,
               0x81430004, 0x71470066, 0x28070000,
               Conditional(core + 0x30, zero_try, 0x41820000)});
    Put(zero_try, {0x815D000C, 0x2B0A0000,
                   Conditional(zero_try + 8, return_one, 0x419A0000)});
    Put(return_one,
        {0x38600001, 0x383F0090, Branch(return_one + 8, restore, false)});
    Put(save, {0xFB61FFD0, 0xFB81FFD8, 0xFBA1FFE0, 0xFBC1FFE8, 0xFBE1FFF0,
               0x9181FFF8, 0x4E800020});
    Put(restore, {0xEB61FFD0, 0xEB81FFD8, 0xEBA1FFE0, 0xEBC1FFE8, 0xEBE1FFF0,
                  0x8181FFF8, 0x7D8803A6, 0x4E800020});
  }

  static uint32_t Branch(uint32_t pc, uint32_t target, bool link) {
    return 0x48000000 | ((target - pc) & 0x03FFFFFC) | uint32_t(link);
  }

  static uint32_t Conditional(uint32_t pc, uint32_t target, uint32_t form) {
    return form | ((target - pc) & 0xFFFC);
  }

  void Put(uint32_t address, std::initializer_list<uint32_t> words) {
    size_t at = address - base;
    for (uint32_t word : words) {
      bytes[at++] = uint8_t(word >> 24);
      bytes[at++] = uint8_t(word >> 16);
      bytes[at++] = uint8_t(word >> 8);
      bytes[at++] = uint8_t(word);
    }
  }

  ExceptionMetadataInput Input() {
    ExceptionMetadataInput input;
    input.image_base = base;
    input.image_end = uint64_t(base) + bytes.size();
    input.sections = sections;
    input.read = [this](uint32_t address, std::span<uint8_t> output) {
      ++read_calls;
      read_bytes += output.size();
      if (read_calls == fail_read) {
        if (throw_read) {
          throw std::runtime_error("synthetic reader failure");
        }
        return false;
      }
      if (address < base ||
          uint64_t(address - base) + output.size() > bytes.size()) {
        return false;
      }
      std::copy_n(bytes.begin() + (address - base), output.size(),
                  output.begin());
      return true;
    };
    return input;
  }

  CppFirstPhaseAdmission Admit(uint32_t flags = 0) {
    return AdmitNoCatchCppFirstPhase(function, flags, Input(), kOwned);
  }
};

void CheckDeclined(const CppFirstPhaseAdmission& admission, Status status) {
  CHECK(admission.status == status);
  CHECK(admission.handler_address == 0);
  CHECK(admission.function_entry_address == 0);
}

TEST_CASE("First-phase no-catch admission follows relocated actual code shapes",
          "[cpu][cpp-first-phase]") {
  for (bool backwards : {false, true}) {
    Fixture fixture(backwards ? 0x82000000 : 0x10000000, backwards);
    const auto admission = fixture.Admit();
    REQUIRE(admission.status == Status::kAdmitted);
    CHECK(admission.handler_address == fixture.wrapper);
    CHECK(admission.function_entry_address == fixture.function.table_address);
    CHECK(fixture.read_calls == 9);
    CHECK(fixture.read_bytes == 224);
  }
}

TEST_CASE("First-phase admission rejects other phases and inconsistent data",
          "[cpu][cpp-first-phase]") {
  for (uint32_t flags : {2U, 4U, 0x20U, 0x40U}) {
    Fixture fixture;
    CheckDeclined(fixture.Admit(flags), Status::kUnsupported);
    CHECK(fixture.read_calls == 0);
  }
  Fixture fixture;
  SECTION("Nonzero try blocks") {
    fixture.Put(fixture.function.handler->data_address + 12, {1});
  }
  SECTION("Unknown FuncInfo format") {
    fixture.Put(fixture.function.handler->data_address, {0x19930521});
  }
  SECTION("Entry does not describe this function") {
    fixture.Put(fixture.function.table_address, {fixture.function.begin + 4});
  }
  SECTION("Prefix handler differs from copied metadata") {
    fixture.Put(fixture.function.begin - 8, {fixture.wrapper + 4});
  }
  SECTION("Prefix FuncInfo differs from copied metadata") {
    fixture.Put(fixture.function.begin - 4,
                {fixture.function.handler->data_address + 4});
  }
  SECTION("Imported C-specific handler belongs to a separate admission route") {
    fixture.function.handler->imported_c_specific = true;
  }
  CheckDeclined(fixture.Admit(), Status::kUnsupported);
}

TEST_CASE("Taken-path instruction mutations cannot add context or other reads",
          "[cpu][cpp-first-phase]") {
  for (size_t mutation = 0; mutation < 9; ++mutation) {
    Fixture fixture;
    const std::array<std::pair<uint32_t, uint32_t>, 9> changes = {{
        {fixture.wrapper, 0x81660008},       // Unknown dispatcher+8.
        {fixture.wrapper + 12, 0x81670010},  // Different FuncInfo field.
        {fixture.core + 20, 0x83850000},     // A new CONTEXT read.
        {fixture.core + 40, 0x71470064},     // A different phase mask.
        {fixture.zero_try, 0x815D0010},      // Different count source.
        {fixture.return_one, 0x38600000},    // Return value is not 1.
        {fixture.save, 0x83660000},          // Helper reads dispatcher.
        {fixture.restore + 24, 0x7D6803A6},  // Wrong LR restore source.
        {fixture.restore + 28, 0x4E800420},  // Indirect continuation.
    }};
    INFO("Mutation " << mutation);
    fixture.Put(changes[mutation].first, {changes[mutation].second});
    CheckDeclined(fixture.Admit(), Status::kUnsupported);
    CHECK(fixture.read_calls <= 9);
  }
}

TEST_CASE("Admission decodes branch forms and rejects unsupported transfers",
          "[cpu][cpp-first-phase]") {
  Fixture fixture;
  SECTION("Absolute wrapper branch") {
    fixture.Put(fixture.wrapper + 0x34, {0x48000002});
  }
  SECTION("Save helper is not linked") {
    fixture.Put(fixture.core + 4,
                {Fixture::Branch(fixture.core + 4, fixture.save, false)});
  }
  SECTION("First-phase conditional uses the wrong CR bit") {
    fixture.Put(fixture.core + 0x30,
                {Fixture::Conditional(fixture.core + 0x30, fixture.zero_try,
                                      0x41860000)});
  }
  SECTION("Zero-try conditional is linked") {
    fixture.Put(fixture.zero_try + 8,
                {Fixture::Conditional(fixture.zero_try + 8, fixture.return_one,
                                      0x419A0001)});
  }
  SECTION("Return tail uses a computed branch") {
    fixture.Put(fixture.return_one + 8, {0x4E800420});
  }
  CheckDeclined(fixture.Admit(), Status::kUnsupported);
}

TEST_CASE("Every failed exact reader call leaves no handler admission",
          "[cpu][cpp-first-phase]") {
  for (size_t failure = 1; failure <= 9; ++failure) {
    Fixture fixture;
    fixture.fail_read = failure;
    CheckDeclined(fixture.Admit(), Status::kReadFailed);
    CHECK(fixture.read_calls == failure);
  }
  Fixture throwing;
  throwing.fail_read = 8;
  throwing.throw_read = true;
  CheckDeclined(throwing.Admit(), Status::kReadFailed);
}

TEST_CASE("Admission requires owned provenance, executable bounds and budget",
          "[cpu][cpp-first-phase]") {
  Fixture fixture;
  auto input = fixture.Input();
  SECTION("Unknown provenance performs no reads") {
    CheckDeclined(AdmitNoCatchCppFirstPhase(fixture.function, 0, input),
                  Status::kUntrustedEvidence);
    CHECK(fixture.read_calls == 0);
  }
  SECTION("Code is not executable") {
    fixture.sections[0].executable = false;
    CheckDeclined(fixture.Admit(), Status::kBounds);
  }
  SECTION("Code section is unavailable") {
    fixture.sections[0].available = false;
    CheckDeclined(fixture.Admit(), Status::kBounds);
  }
  SECTION("Misaligned code target") {
    fixture.function.handler->address += 2;
    fixture.Put(fixture.function.begin - 8, {fixture.wrapper + 2});
    CheckDeclined(fixture.Admit(), Status::kBounds);
  }
  SECTION("Read budget cannot cover the final helper") {
    input.limits.max_read_bytes = 223;
    CheckDeclined(AdmitNoCatchCppFirstPhase(fixture.function, 0, input, kOwned),
                  Status::kResourceLimit);
    CHECK(fixture.read_calls == 8);
  }
  SECTION("Section scans are bounded before reading") {
    std::vector<ExceptionMetadataSection> excessive(257, fixture.sections[0]);
    input.sections = excessive;
    CheckDeclined(AdmitNoCatchCppFirstPhase(fixture.function, 0, input, kOwned),
                  Status::kResourceLimit);
    CHECK(fixture.read_calls == 0);
  }
  SECTION("A branch cannot wrap above the guest address space") {
    Fixture high(0xFFFFC000);
    REQUIRE(high.Admit().status == Status::kAdmitted);
    high.Put(high.wrapper + 0x34, {0x49FFFFFC});
    CheckDeclined(high.Admit(), Status::kUnsupported);
  }
}

}  // namespace
}  // namespace xe::cpu::ppc::test
