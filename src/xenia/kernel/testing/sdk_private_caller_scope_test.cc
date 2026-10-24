// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect after validation.
// Never upstream; see SDK_DERIVED_PRIVATE.md and CONTRIBUTING.md.

#include "xenia/kernel/sdk_private_first_chance.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/exception_metadata.h"

namespace xe::kernel::sdk_private::test {
namespace {

struct Fixture {
  std::array<uint8_t, 0x200> bytes{};
  std::array<cpu::ExceptionMetadataSection, 1> sections{};
  cpu::ExceptionMetadataSnapshot metadata;
  size_t reads = 0;

  Fixture() {
    sections[0].begin = 0x1000;
    sections[0].end = 0x1200;
    sections[0].readable = true;
    sections[0].executable = true;
    // bl 0x1100 at 0x103C; the saved LR is 0x1040.
    Put(0x103C, 0x480000C5);
    metadata.lookup_valid = true;
    metadata.status = cpu::ExceptionMetadataStatus::kValid;
    cpu::ExceptionMetadataFunction function;
    function.begin = 0x1000;
    function.end = 0x1080;
    function.packed_candidate = true;
    function.status = cpu::ExceptionMetadataStatus::kValid;
    function.handler.emplace();
    function.handler->status = cpu::ExceptionMetadataStatus::kValid;
    function.handler->imported_c_specific = true;
    cpu::ExceptionMetadataScope scope;
    scope.begin = 0x1020;
    scope.end = 0x1040;
    scope.filter_or_handler = 0x1060;
    scope.jump_target = 0x1070;
    scope.status = cpu::ExceptionMetadataStatus::kValid;
    function.handler->scopes.push_back(scope);
    metadata.functions.push_back(function);
  }

  void Put(uint32_t address, uint32_t word) {
    const size_t offset = address - 0x1000;
    for (size_t i = 0; i < 4; ++i) {
      bytes[offset + i] = uint8_t(word >> (24 - i * 8));
    }
  }

  cpu::ExceptionMetadataInput Input() {
    cpu::ExceptionMetadataInput input;
    input.image_base = 0x1000;
    input.image_end = 0x1200;
    input.sections = sections;
    input.read = [this](uint32_t address, std::span<uint8_t> output) {
      ++reads;
      if (address < 0x1000 || uint64_t(address) + output.size() > 0x1200) {
        return false;
      }
      std::copy_n(bytes.begin() + (address - 0x1000), output.size(),
                  output.begin());
      return true;
    };
    return input;
  }
};

}  // namespace

TEST_CASE("Private caller scope includes a call whose saved LR ends the scope",
          "[sdk-private-caller-scope]") {
  Fixture fixture;
  const uint32_t raw_lr = 0x1040;
  CHECK(fixture.metadata.LookupFrame(raw_lr).matching_scopes.empty());
  const auto pc = SelectSuspendedCallSite(raw_lr, 0x1100, fixture.Input());
  REQUIRE(pc);
  CHECK(*pc == 0x103C);
  CHECK(fixture.reads == 1);
  const auto caller = fixture.metadata.LookupFrame(*pc);
  REQUIRE(caller.matching_scopes.size() == 1);
  CHECK(caller.matching_scopes[0].filter_or_handler == 0x1060);
}

TEST_CASE("Private caller scope cannot select the adjacent return-site scope",
          "[sdk-private-caller-scope]") {
  Fixture fixture;
  auto adjacent = fixture.metadata.functions[0].handler->scopes[0];
  adjacent.begin = 0x1040;
  adjacent.end = 0x1060;
  adjacent.filter_or_handler = 0x1064;
  fixture.metadata.functions[0].handler->scopes.push_back(adjacent);
  REQUIRE(fixture.metadata.LookupFrame(0x1040).matching_scopes.size() == 1);
  CHECK(fixture.metadata.LookupFrame(0x1040)
            .matching_scopes[0]
            .filter_or_handler == 0x1064);
  const auto pc = SelectSuspendedCallSite(0x1040, 0x1100, fixture.Input());
  REQUIRE(pc);
  const auto caller = fixture.metadata.LookupFrame(*pc);
  REQUIRE(caller.matching_scopes.size() == 1);
  CHECK(caller.matching_scopes[0].filter_or_handler == 0x1060);
}

TEST_CASE("Private caller scope rejects invalid addresses before reading",
          "[sdk-private-caller-scope]") {
  Fixture fixture;
  for (uint32_t lr : {0u, 3u, 0x1041u, 0x1000u, 0x1204u, 0xFFFFFFFCu}) {
    CHECK_FALSE(SelectSuspendedCallSite(lr, 0x1100, fixture.Input()));
  }
  CHECK(fixture.reads == 0);
  for (uint32_t callee : {0u, 0x1101u, 0x1200u}) {
    CHECK_FALSE(SelectSuspendedCallSite(0x1040, callee, fixture.Input()));
  }
  CHECK(fixture.reads == 0);
  fixture.sections[0].executable = false;
  CHECK_FALSE(SelectSuspendedCallSite(0x1040, 0x1100, fixture.Input()));
  CHECK(fixture.reads == 0);
  fixture.sections[0].executable = true;
  fixture.sections[0].available = false;
  CHECK_FALSE(SelectSuspendedCallSite(0x1040, 0x1100, fixture.Input()));
  CHECK(fixture.reads == 0);
}

TEST_CASE("Private caller scope requires the admitted linked target",
          "[sdk-private-caller-scope]") {
  Fixture fixture;
  for (uint32_t word : {0x480000C4u, 0x480000C7u, 0x4E800421u, 0x60000000u,
                        0x48000000u, 0x480000C1u}) {
    fixture.Put(0x103C, word);
    CHECK_FALSE(SelectSuspendedCallSite(0x1040, 0x1100, fixture.Input()));
  }
  fixture.Put(0x113C, 0x4BFFFEC5);  // bl backwards to 0x1000.
  CHECK(SelectSuspendedCallSite(0x1140, 0x1000, fixture.Input()) == 0x113C);
  auto input = fixture.Input();
  input.read = [](uint32_t, std::span<uint8_t>) { return false; };
  CHECK_FALSE(SelectSuspendedCallSite(0x1140, 0x1000, input));
}

}  // namespace xe::kernel::sdk_private::test
