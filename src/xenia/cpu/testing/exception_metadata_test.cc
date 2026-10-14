/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/exception_metadata.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

namespace xe::cpu::test {
namespace {

using Status = ExceptionMetadataStatus;

TEST_CASE("Import metadata has no indeterminate absent thunk",
          "[cpu][exception-metadata]") {
  XexModule::ImportLibraryFn import;
  CHECK(import.ordinal == 0);
  CHECK(import.value_address == 0);
  CHECK(import.thunk_address == 0);
}

struct Fixture {
  uint32_t base = 0x10000000;
  uint64_t end = uint64_t(base) + 0x1000;
  std::vector<ExceptionMetadataSection> sections;
  std::vector<ExceptionMetadataImport> imports;
  std::map<uint32_t, uint8_t> bytes;
  std::vector<std::pair<uint32_t, size_t>> reads;
  bool refused_read = false;

  explicit Fixture(uint32_t relocated_base = 0x10000000)
      : base(relocated_base), end(uint64_t(base) + 0x1000) {
    sections = {
        {".pdata", base, uint64_t(base) + 24, true, false, 0},
        {".rdata", base + 0x100, uint64_t(base) + 0x300, true, false, 0},
        {".text", base + 0x400, end, true, true, 0}};
    imports = {{"xboxkrnl.exe", 0x1A5, base + 0x700}};
    Entry(0, base + 0x400, 0x40000401);
    Entry(1, base + 0x450, 0xC0001002);
    Entry(2, base + 0x700, 0x40000400);
    Word(base + 0x448, base + 0x700);
    Word(base + 0x44C, base + 0x100);
    Word(base + 0x100, 1);
    Scope(base + 0x104, base + 0x454, base + 0x480, base + 0x740, base + 0x484);
  }

  void Word(uint32_t address, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
      bytes[address + i] = uint8_t(value >> (24 - 8 * i));
    }
  }

  void Entry(size_t index, uint32_t begin, uint32_t raw) {
    Word(base + uint32_t(index * 8), begin);
    Word(base + uint32_t(index * 8) + 4, raw);
  }

  void Scope(uint32_t address, uint32_t begin, uint32_t end, uint32_t filter,
             uint32_t jump) {
    Word(address, begin);
    Word(address + 4, end);
    Word(address + 8, filter);
    Word(address + 12, jump);
  }

  ExceptionMetadataInput Input() {
    ExceptionMetadataInput input;
    input.image_base = base;
    input.image_end = end;
    input.sections = sections;
    input.imports = imports;
    input.read = [this](uint32_t address, std::span<uint8_t> output) {
      reads.emplace_back(address, output.size());
      const bool allowed = std::any_of(
          sections.begin(), sections.end(), [&](const auto& section) {
            return section.readable && address >= section.begin &&
                   uint64_t(address) + output.size() <= end &&
                   uint64_t(address) + output.size() <= section.end;
          });
      CHECK(allowed);
      if (!allowed || refused_read) {
        return false;
      }
      for (size_t i = 0; i < output.size(); ++i) {
        auto it = bytes.find(uint32_t(uint64_t(address) + i));
        output[i] = it == bytes.end() ? 0 : it->second;
      }
      return true;
    };
    return input;
  }
};

TEST_CASE("Exception metadata describes relocated packed frames and scopes",
          "[cpu][exception-metadata]") {
  for (uint32_t base : {0x10000000u, 0x82000000u}) {
    Fixture fixture(base);
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    REQUIRE(snapshot.status == Status::kValid);
    REQUIRE(snapshot.functions.size() == 3);
    const auto leaf = snapshot.LookupFrame(base + 0x40C);
    REQUIRE(leaf.function);
    CHECK(leaf.function->begin == base + 0x400);
    CHECK(leaf.function->end == uint64_t(base) + 0x410);
    CHECK_FALSE(leaf.function->handler);
    const auto frame = snapshot.LookupFrame(base + 0x460);
    REQUIRE(frame.function);
    REQUIRE(frame.function->handler);
    CHECK(frame.function->raw_info == 0xC0001002);
    CHECK(frame.function->handler->imported_c_specific);
    CHECK(frame.function->handler->status == Status::kValid);
    REQUIRE(frame.matching_scopes.size() == 1);
    CHECK(frame.matching_scopes[0].filter_or_handler == base + 0x740);
    CHECK(frame.matching_scopes[0].jump_target == base + 0x484);
    CHECK(snapshot.LookupFrame(base + 0x453).status == Status::kMissing);
    CHECK(snapshot.LookupFrame(base + 0x454).matching_scopes.size() == 1);
    CHECK(snapshot.LookupFrame(base + 0x480).matching_scopes.empty());
    CHECK(snapshot.LookupFrame(base + 0x490).status == Status::kMissing);
    CHECK(snapshot.LookupFrame(base + 0x440).status == Status::kMissing);
    CHECK(snapshot.LookupFrame(base - 4).status == Status::kMissing);
    const std::array<uint32_t, 3> pcs{base + 0x460, base + 0x404, base + 0x440};
    const auto frames = snapshot.DescribeFrames(pcs);
    CHECK(frames[0].pc == pcs[0]);
    CHECK(frames[1].pc == pcs[1]);
    CHECK(frames[2].status == Status::kMissing);
    CHECK(fixture.reads.front().first == base);
    CHECK(fixture.reads.front().second == 24);
  }
}

TEST_CASE("Owned exception snapshot does not retain reader or input pointers",
          "[cpu][exception-metadata]") {
  ExceptionMetadataSnapshot snapshot;
  {
    Fixture fixture;
    snapshot = ParseExceptionMetadata(fixture.Input());
    fixture.bytes.clear();
    fixture.sections.clear();
    fixture.imports.clear();
  }
  const auto frame = snapshot.LookupFrame(0x10000460);
  REQUIRE(frame.function);
  REQUIRE(frame.function->handler);
  CHECK(frame.function->handler->address == 0x10000700);
  CHECK(frame.matching_scopes.size() == 1);
  CHECK(snapshot.imports[0].library == "xboxkrnl.exe");
}

TEST_CASE("Bit30-clear helper forms never read a handler prefix",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  fixture.base = 0x82000000;
  fixture.end = 0x83000000;
  fixture.sections = {
      {".pdata", fixture.base, uint64_t(fixture.base) + 48, true, false, 0},
      {".text", 0x822D0000, 0x82D00000, true, true, 0}};
  fixture.bytes.clear();
  fixture.imports.clear();
  constexpr std::array<std::pair<uint32_t, uint32_t>, 6> helpers{
      {{0x827DE310, 0x00001414},
       {0x827DE360, 0x80001515},
       {0x827E0330, 0x00001313},
       {0x827E037C, 0x80001313},
       {0x82C60800, 0x0000A6A6},
       {0x82C60A98, 0x8000A6A6}}};
  for (size_t i = 0; i < helpers.size(); ++i) {
    fixture.Entry(i, helpers[i].first, helpers[i].second);
  }
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  REQUIRE(snapshot.status == Status::kValid);
  REQUIRE(snapshot.functions.size() == helpers.size());
  REQUIRE(fixture.reads.size() == 1);
  CHECK(fixture.reads[0].second == 48);
  for (const auto& [begin, info] : helpers) {
    const auto frame = snapshot.LookupFrame(begin);
    REQUIRE(frame.function);
    CHECK(frame.status == Status::kOpaque);
    CHECK(frame.function->raw_info == info);
    CHECK_FALSE(frame.function->handler);
    CHECK(snapshot.LookupFrame(begin + 4).status == Status::kMissing);
  }
}

TEST_CASE("Unknown handlers and sentinel scope roles stay uninterpreted",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  SECTION("No import identity") {
    fixture.imports.clear();
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    REQUIRE(snapshot.functions[1].handler);
    CHECK(snapshot.functions[1].handler->status == Status::kOpaque);
    CHECK(snapshot.functions[1].handler->scopes.empty());
    CHECK(fixture.reads.size() == 2);
  }
  SECTION("Conflicting identity") {
    fixture.imports.push_back({"xam.xex", 0x1A5, fixture.base + 0x700});
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    CHECK_FALSE(snapshot.functions[1].handler->imported_c_specific);
  }
  SECTION("Missing thunk") {
    fixture.imports[0].thunk_address = 0;
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    CHECK_FALSE(snapshot.functions[1].handler->imported_c_specific);
  }
  SECTION("Case-insensitive kernel name") {
    fixture.imports[0].library = "XBOXKRNL.EXE";
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    CHECK(snapshot.functions[1].handler->imported_c_specific);
  }
  SECTION("Zero jump and sentinel filter") {
    fixture.Scope(fixture.base + 0x104, fixture.base + 0x454,
                  fixture.base + 0x480, UINT32_MAX, 0);
    const auto frame = ParseExceptionMetadata(fixture.Input())
                           .LookupFrame(fixture.base + 0x460);
    REQUIRE(frame.matching_scopes.size() == 1);
    CHECK(frame.matching_scopes[0].opaque_roles);
    CHECK(frame.matching_scopes[0].filter_or_handler == UINT32_MAX);
    CHECK(frame.matching_scopes[0].jump_target == 0);
  }
}

TEST_CASE("Metadata rejects malformed ranges without unbounded reads",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  SECTION("Overlapping sections") {
    fixture.sections[1].begin = fixture.base + 8;
  }
  SECTION("Usable extent beyond image") { fixture.sections[2].end += 4; }
  SECTION("Unrelated usable extent beyond image") {
    fixture.sections.push_back({".reloc", fixture.base + 0xF00,
                                uint64_t(fixture.base) + 0x1200, true, false,
                                0});
  }
  SECTION("Guest-address overflow") { fixture.end = uint64_t{1} << 33; }
  SECTION("Partial table") { fixture.sections[0].end += 1; }
  SECTION("Unaligned table") { fixture.sections[0].begin += 1; }
  SECTION("Unreadable table") { fixture.sections[0].readable = false; }
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  CHECK(snapshot.status == Status::kInvalid);
  CHECK_FALSE(snapshot.lookup_valid);
  CHECK(fixture.reads.empty());
}

TEST_CASE(
    "Unavailable unrelated sections do not authorize reads or poison pdata",
    "[cpu][exception-metadata]") {
  Fixture fixture;
  fixture.sections.push_back(
      {".reloc", fixture.base + 0xF00, uint64_t(fixture.base) + 0x1200, true,
       false, 0, false, "Declared extent is outside loaded image"});
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  REQUIRE(snapshot.status == Status::kValid);
  REQUIRE(snapshot.sections.size() == 4);
  CHECK_FALSE(snapshot.sections.back().available);
  CHECK(snapshot.sections.back().end == uint64_t(fixture.base) + 0x1200);
  CHECK_FALSE(snapshot.sections.back().availability_diagnostic.empty());
  CHECK_FALSE(snapshot.diagnostics.empty());
  CHECK(snapshot.LookupFrame(fixture.base + 0x460).matching_scopes.size() == 1);
  for (const auto& [address, size] : fixture.reads) {
    CHECK(uint64_t(address) + size <= fixture.end);
  }
  fixture.sections[0].end = uint64_t(fixture.base) + 0x1200;
  fixture.reads.clear();
  const auto unavailable_table = ParseExceptionMetadata(fixture.Input());
  CHECK(unavailable_table.status == Status::kInvalid);
  CHECK_FALSE(unavailable_table.lookup_valid);
  CHECK(fixture.reads.empty());
}

TEST_CASE("Absent table and raw padding do not create metadata entries",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  SECTION("Missing table") {
    fixture.sections.erase(fixture.sections.begin());
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    CHECK(snapshot.status == Status::kMissing);
    CHECK(snapshot.functions.empty());
    CHECK(fixture.reads.empty());
  }
  SECTION("File padding outside virtual extent") {
    fixture.Entry(3, fixture.base + 0x800, 0x40000400);
    const auto snapshot = ParseExceptionMetadata(fixture.Input());
    REQUIRE(snapshot.functions.size() == 3);
    CHECK(snapshot.LookupFrame(fixture.base + 0x800).status ==
          Status::kMissing);
    CHECK(fixture.reads.front().second == 24);
  }
  SECTION("Duplicate table descriptor") {
    fixture.sections.push_back(fixture.sections.front());
    CHECK(ParseExceptionMetadata(fixture.Input()).status == Status::kInvalid);
    CHECK(fixture.reads.empty());
  }
}

TEST_CASE("Packed handler prefix underflow does not issue a read",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  fixture.base = 0;
  fixture.end = 0x1000;
  fixture.sections = {{".text", 0, 0x100, true, true, 0},
                      {".pdata", 0x200, 0x208, true, false, 0}};
  fixture.bytes.clear();
  fixture.imports.clear();
  fixture.Word(0x200, 4);
  fixture.Word(0x204, 0xC0000400);
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  REQUIRE(snapshot.status == Status::kValid);
  REQUIRE(snapshot.functions[0].handler);
  CHECK(snapshot.functions[0].handler->status == Status::kInvalid);
  REQUIRE(fixture.reads.size() == 1);
  CHECK(fixture.reads[0].first == 0x200);
}

TEST_CASE("Loaded-memory adapter enforces commitment and allocation boundaries",
          "[cpu][exception-metadata][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t base = memory.SystemHeapAlloc(0x3000, 0x1000);
  REQUIRE(base);
  auto* heap = memory.LookupHeap(base);
  REQUIRE(heap);
  auto store = [&](uint32_t address, uint32_t value) {
    auto* output = memory.TranslateVirtual(address);
    for (uint32_t i = 0; i < 4; ++i) {
      output[i] = uint8_t(value >> (24 - 8 * i));
    }
  };
  store(base, base + 0x1000);
  store(base + 4, 0x40000400);
  std::vector<ExceptionMetadataSection> sections{
      {".pdata", base, uint64_t(base) + 8, true, false, 0},
      {".text", base + 0x1000, uint64_t(base) + 0x2000, true, true, 0},
      {".reloc", base + 0x2000, uint64_t(base) + 0x4000, true, false, 0}};
  const auto snapshot = XexModule::ReadExceptionMetadataSnapshot(
      &memory, base, uint64_t(base) + 0x3000, sections, {});
  REQUIRE(snapshot.status == Status::kValid);
  CHECK(snapshot.LookupFrame(base + 0x100C).status == Status::kValid);
  CHECK_FALSE(snapshot.sections[2].available);
  CHECK(snapshot.sections[2].end == uint64_t(base) + 0x4000);
  REQUIRE(heap->Protect(base, 0x1000, 0));
  const auto no_access = XexModule::ReadExceptionMetadataSnapshot(
      &memory, base, uint64_t(base) + 0x3000, sections, {});
  CHECK(no_access.status == Status::kInvalid);
  CHECK_FALSE(no_access.sections[0].available);
  REQUIRE(
      heap->Protect(base, 0x1000, kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(heap->Decommit(base, 0x1000));
  CHECK(XexModule::ReadExceptionMetadataSnapshot(
            &memory, base, uint64_t(base) + 0x3000, sections, {})
            .status == Status::kInvalid);
  memory.SystemHeapFree(base);
}

TEST_CASE("All virtual records are retained without a terminator heuristic",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  SECTION("Zero middle record") { fixture.Entry(1, 0, 0); }
  SECTION("Non-executable middle record") {
    fixture.Entry(1, fixture.base + 0x100, 0x40000400);
  }
  SECTION("Duplicate start") {
    fixture.Entry(1, fixture.base + 0x400, 0x40000400);
  }
  SECTION("Unordered starts") {
    fixture.Entry(0, fixture.base + 0x700, 0x40000400);
  }
  SECTION("Prologue beyond length") {
    fixture.Entry(0, fixture.base + 0x400, 0x40000405);
  }
  SECTION("Zero length") { fixture.Entry(0, fixture.base + 0x400, 0x40000000); }
  SECTION("Function crosses next start") {
    fixture.Entry(0, fixture.base + 0x400, 0x40003000);
  }
  SECTION("Length beyond section") {
    fixture.Entry(2, fixture.base + 0x700, 0x7FFFFF00);
  }
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  CHECK(snapshot.status == Status::kInvalid);
  CHECK_FALSE(snapshot.lookup_valid);
  REQUIRE(snapshot.functions.size() == 3);
  CHECK(snapshot.functions.back().begin == fixture.base + 0x700);
  CHECK(fixture.reads.front().second == 24);
  CHECK(snapshot.LookupFrame(fixture.base + 0x704).status == Status::kInvalid);
}

TEST_CASE("Invalid handler descriptions do not create matching scopes",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  SECTION("Handler in data") {
    fixture.Word(fixture.base + 0x448, fixture.base + 0x100);
  }
  SECTION("Unaligned handler") {
    fixture.Word(fixture.base + 0x448, fixture.base + 0x701);
  }
  SECTION("Metadata in a gap") {
    fixture.Word(fixture.base + 0x44C, fixture.base + 0x350);
  }
  SECTION("Count cannot fit readable extent") {
    fixture.Word(fixture.base + 0x100, UINT32_MAX);
  }
  SECTION("Scope outside its function") {
    fixture.Word(fixture.base + 0x104, fixture.base + 0x400);
  }
  SECTION("Scope end before begin") {
    fixture.Word(fixture.base + 0x108, fixture.base + 0x450);
  }
  SECTION("Filter outside executable ranges") {
    fixture.Word(fixture.base + 0x10C, fixture.base + 0x100);
  }
  const auto frame =
      ParseExceptionMetadata(fixture.Input()).LookupFrame(fixture.base + 0x460);
  REQUIRE(frame.function);
  REQUIRE(frame.function->handler);
  CHECK(frame.function->handler->status == Status::kInvalid);
  CHECK(frame.matching_scopes.empty());
}

TEST_CASE("Metadata resource limits are distinct from invalid metadata",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  auto input = fixture.Input();
  SECTION("Function count") {
    input.limits.max_functions = 2;
    CHECK(ParseExceptionMetadata(input).status == Status::kResourceLimit);
    CHECK(fixture.reads.empty());
  }
  SECTION("Read budget") {
    input.limits.max_read_bytes = 23;
    CHECK(ParseExceptionMetadata(input).status == Status::kResourceLimit);
    CHECK(fixture.reads.empty());
  }
  SECTION("Scope count") {
    input.limits.max_scopes_per_handler = 0;
    const auto snapshot = ParseExceptionMetadata(input);
    CHECK(snapshot.status == Status::kValid);
    CHECK(snapshot.functions[1].handler->status == Status::kResourceLimit);
    CHECK(snapshot.LookupFrame(fixture.base + 0x460).matching_scopes.empty());
  }
  SECTION("Reader declines committed-page check") {
    fixture.refused_read = true;
    CHECK(ParseExceptionMetadata(input).status == Status::kInvalid);
  }
}

TEST_CASE("Packed function may end at the guest address-space boundary",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  fixture.base = 0xFFFFF000;
  fixture.end = uint64_t{1} << 32;
  fixture.sections = {
      {".pdata", fixture.base, uint64_t(fixture.base) + 8, true, false, 0},
      {".text", 0xFFFFFFC0, fixture.end, true, true, 0}};
  fixture.bytes.clear();
  fixture.imports.clear();
  fixture.Entry(0, 0xFFFFFFF0, 0x40000400);
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  CHECK(snapshot.status == Status::kValid);
  CHECK(snapshot.LookupFrame(0xFFFFFFFC).status == Status::kValid);
  CHECK(snapshot.functions[0].end == uint64_t{1} << 32);
  CHECK(snapshot.LookupFrame(0).status == Status::kMissing);
}

TEST_CASE("World metadata fixture describes leaf opaque caller and outer scope",
          "[cpu][exception-metadata]") {
  Fixture fixture;
  fixture.base = 0x82000000;
  fixture.end = 0x83000000;
  fixture.sections = {{".rdata", fixture.base, 0x8225E800, true, false, 0},
                      {".pdata", 0x8225E800, 0x8225E818, true, false, 0},
                      {".text", 0x822D0000, 0x82D00000, true, true, 0}};
  fixture.bytes.clear();
  fixture.imports = {{"xboxkrnl.exe", 0x1A5, 0x82CE7E8C}};
  for (const auto& [address, value] :
       std::array<std::pair<uint32_t, uint32_t>, 10>{
           {{0x8225E800, 0x82311A68},
            {0x8225E804, 0x40001103},
            {0x8225E808, 0x82313028},
            {0x8225E80C, 0xC0001305},
            {0x8225E810, 0x827FC450},
            {0x8225E814, 0xC0006E04},
            {0x82313020, 0x827DEF10},
            {0x82313024, 0x820048D8},
            {0x827FC448, 0x82CE7E8C},
            {0x827FC44C, 0x8225E380}}}) {
    fixture.Word(address, value);
  }
  fixture.Word(0x8225E380, 1);
  fixture.Scope(0x8225E384, 0x827FC46C, 0x827FC5F8, 0x827FC608, 0x827FC604);
  const auto snapshot = ParseExceptionMetadata(fixture.Input());
  REQUIRE(snapshot.status == Status::kValid);
  const std::array<uint32_t, 3> pcs{0x82311A94, 0x82313054, 0x827FC5E0};
  const auto frames = snapshot.DescribeFrames(pcs);
  REQUIRE(frames[0].function);
  CHECK(frames[0].function->raw_info == 0x40001103);
  CHECK_FALSE(frames[0].function->handler);
  REQUIRE(frames[1].function->handler);
  CHECK(frames[1].function->handler->status == Status::kOpaque);
  CHECK(frames[1].function->handler->data_address == 0x820048D8);
  REQUIRE(frames[2].matching_scopes.size() == 1);
  CHECK(frames[2].matching_scopes[0].filter_or_handler == 0x827FC608);
  CHECK(frames[2].matching_scopes[0].jump_target == 0x827FC604);
}

}  // namespace
}  // namespace xe::cpu::test
