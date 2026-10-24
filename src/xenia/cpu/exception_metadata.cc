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
#include <iterator>
#include <string_view>
#include <utility>

namespace xe::cpu {
namespace {

constexpr uint64_t kGuestAddressEnd = uint64_t{1} << 32;
constexpr uint32_t kPackedForm = 0x40000000;
constexpr uint32_t kHandlerPrefix = 0x80000000;

uint32_t ReadBe32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

bool IsXboxKernel(std::string_view name) {
  // SetupLibraryImports supplies the basename; raw inputs may use the full
  // name.
  constexpr std::string_view basename = "xboxkrnl";
  constexpr std::string_view fullname = "xboxkrnl.exe";
  const auto expected = name.size() == basename.size() ? basename : fullname;
  if (name.size() != expected.size()) {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      c = char(c + ('a' - 'A'));
    }
    if (c != expected[i]) {
      return false;
    }
  }
  return true;
}

bool IsOpaqueRole(uint32_t value) { return value <= 1 || value == UINT32_MAX; }

class Parser {
 public:
  explicit Parser(const ExceptionMetadataInput& input) : input_(input) {
    result_.image_base = input.image_base;
    result_.image_end = input.image_end;
    result_.sections.assign(input.sections.begin(), input.sections.end());
    result_.imports.assign(input.imports.begin(), input.imports.end());
  }

  ExceptionMetadataSnapshot Parse() {
    if (input_.image_end <= input_.image_base ||
        input_.image_end > kGuestAddressEnd || !input_.read) {
      return Invalid("Invalid image extent or reader");
    }
    read_budget_ = std::min(input_.limits.max_read_bytes,
                            input_.image_end - input_.image_base);
    std::vector<const ExceptionMetadataSection*> ordered;
    const ExceptionMetadataSection* pdata = nullptr;
    for (auto& section : result_.sections) {
      if (section.begin < input_.image_base || section.end <= section.begin ||
          section.end > input_.image_end) {
        if (section.available) {
          return Invalid("Invalid supplied usable section extent");
        }
        if (section.availability_diagnostic.empty()) {
          section.availability_diagnostic =
              "Declared extent is outside loaded image";
        }
      }
      if (section.name == ".pdata") {
        if (pdata) {
          return Invalid("Multiple .pdata sections");
        }
        pdata = &section;
      }
      if (section.available) {
        ordered.push_back(&section);
      } else {
        if (section.availability_diagnostic.empty()) {
          section.availability_diagnostic = "Declared range marked unavailable";
        }
        result_.diagnostics.push_back(section.name + ": " +
                                      section.availability_diagnostic);
      }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](auto* a, auto* b) { return a->begin < b->begin; });
    for (size_t i = 1; i < ordered.size(); ++i) {
      if (ordered[i - 1]->end > ordered[i]->begin) {
        return Invalid("Overlapping loaded sections");
      }
    }
    if (!pdata) {
      return std::move(result_);
    }
    if (!pdata->available || !pdata->readable || (pdata->begin & 3)) {
      return Invalid("Unaligned, partial or unreadable .pdata");
    }
    const uint64_t table_size = pdata->end - pdata->begin;
    if (table_size & 7) {
      return Invalid("Partial .pdata record");
    }
    if (table_size / 8 > input_.limits.max_functions ||
        table_size > read_budget_ || table_size > SIZE_MAX) {
      result_.status = ExceptionMetadataStatus::kResourceLimit;
      result_.diagnostics.emplace_back("Runtime-function table resource limit");
      return std::move(result_);
    }
    std::vector<uint8_t> table(static_cast<size_t>(table_size));
    if (!Read(pdata->begin, table)) {
      result_.status = read_status_;
      result_.diagnostics.emplace_back("Could not read .pdata");
      return std::move(result_);
    }
    bool ordered_starts = true;
    for (size_t i = 0; i < table.size(); i += 8) {
      ExceptionMetadataFunction function;
      function.table_address = uint32_t(uint64_t(pdata->begin) + i);
      function.begin = ReadBe32(table.data() + i);
      function.raw_info = ReadBe32(table.data() + i + 4);
      function.packed_candidate = (function.raw_info & kPackedForm) != 0;
      if (!result_.functions.empty() &&
          function.begin <= result_.functions.back().begin) {
        ordered_starts = false;
      }
      if (!function.begin || (function.begin & 3) ||
          !Containing(function.begin, 4, false, true)) {
        function.status = ExceptionMetadataStatus::kInvalid;
      } else if (function.packed_candidate) {
        function.prologue_instructions = function.raw_info & 0xFF;
        function.length_instructions = (function.raw_info >> 8) & 0x3FFFFF;
        function.end = uint64_t(function.begin) +
                       uint64_t(function.length_instructions) * 4;
        function.status =
            function.length_instructions &&
                    function.prologue_instructions <=
                        function.length_instructions &&
                    Containing(function.begin, function.end - function.begin,
                               false, true)
                ? ExceptionMetadataStatus::kValid
                : ExceptionMetadataStatus::kInvalid;
      }
      result_.functions.push_back(std::move(function));
    }
    bool valid_index = ordered_starts;
    for (size_t i = 0; i < result_.functions.size(); ++i) {
      auto& function = result_.functions[i];
      if (function.status == ExceptionMetadataStatus::kValid &&
          i + 1 < result_.functions.size() &&
          function.end > result_.functions[i + 1].begin) {
        function.status = ExceptionMetadataStatus::kInvalid;
      }
      if (function.status == ExceptionMetadataStatus::kInvalid) {
        valid_index = false;
      }
      if (function.status == ExceptionMetadataStatus::kValid &&
          (function.raw_info & kHandlerPrefix)) {
        function.handler = DescribeHandler(function);
      }
    }
    result_.lookup_valid = valid_index;
    result_.status = valid_index ? ExceptionMetadataStatus::kValid
                                 : ExceptionMetadataStatus::kInvalid;
    if (!valid_index) {
      result_.diagnostics.emplace_back(
          "Invalid, overlapping, duplicate or unordered runtime-function "
          "records");
    }
    return std::move(result_);
  }

 private:
  ExceptionMetadataSnapshot Invalid(const char* message) {
    result_.status = ExceptionMetadataStatus::kInvalid;
    result_.diagnostics.emplace_back(message);
    return std::move(result_);
  }

  const ExceptionMetadataSection* Containing(uint32_t address, uint64_t length,
                                             bool readable,
                                             bool executable) const {
    if (!length || length > kGuestAddressEnd - address) {
      return nullptr;
    }
    for (const auto& section : result_.sections) {
      if (section.available && address >= section.begin &&
          uint64_t(address) + length <= section.end &&
          (!readable || section.readable) &&
          (!executable || section.executable)) {
        return &section;
      }
    }
    return nullptr;
  }

  bool Read(uint32_t address, std::span<uint8_t> output) {
    read_status_ = ExceptionMetadataStatus::kInvalid;
    if (!Containing(address, output.size(), true, false)) {
      return false;
    }
    if (output.size() > read_budget_) {
      read_status_ = ExceptionMetadataStatus::kResourceLimit;
      return false;
    }
    if (!input_.read(address, output)) {
      return false;
    }
    read_budget_ -= output.size();
    return true;
  }

  bool IsCSpecificImport(uint32_t address) const {
    bool matched = false;
    for (const auto& import : result_.imports) {
      if (!import.thunk_address || import.thunk_address != address) {
        continue;
      }
      if (!IsXboxKernel(import.library) || import.ordinal != 0x1A5) {
        return false;  // Conflicting import identity stays opaque.
      }
      matched = true;
    }
    return matched;
  }

  ExceptionMetadataHandler DescribeHandler(
      const ExceptionMetadataFunction& function) {
    ExceptionMetadataHandler handler;
    std::array<uint8_t, 8> prefix;
    if (function.begin < prefix.size()) {
      handler.status = ExceptionMetadataStatus::kInvalid;
      return handler;
    }
    if (!Read(function.begin - uint32_t(prefix.size()), prefix)) {
      handler.status = read_status_;
      return handler;
    }
    handler.address = ReadBe32(prefix.data());
    handler.data_address = ReadBe32(prefix.data() + 4);
    if ((handler.address & 3) || (handler.data_address & 3) ||
        !Containing(handler.address, 4, false, true) ||
        !Containing(handler.data_address, 4, true, false)) {
      handler.status = ExceptionMetadataStatus::kInvalid;
      return handler;
    }
    handler.imported_c_specific = IsCSpecificImport(handler.address);
    if (!handler.imported_c_specific) {
      return handler;
    }
    std::array<uint8_t, 4> header;
    if (!Read(handler.data_address, header)) {
      handler.status = read_status_;
      return handler;
    }
    const uint32_t count = ReadBe32(header.data());
    const uint64_t table_size = uint64_t(count) * 16;
    if (!Containing(handler.data_address, 4 + table_size, true, false)) {
      handler.status = ExceptionMetadataStatus::kInvalid;
      return handler;
    }
    if (count > input_.limits.max_scopes_per_handler ||
        table_size > read_budget_ || table_size > SIZE_MAX) {
      handler.status = ExceptionMetadataStatus::kResourceLimit;
      return handler;
    }
    if (!count) {
      handler.status = ExceptionMetadataStatus::kValid;
      return handler;
    }
    std::vector<uint8_t> table(static_cast<size_t>(table_size));
    if (!Read(handler.data_address + 4, table)) {
      handler.status = read_status_;
      return handler;
    }
    handler.status = ExceptionMetadataStatus::kValid;
    for (size_t i = 0; i < table.size(); i += 16) {
      ExceptionMetadataScope scope;
      scope.begin = ReadBe32(table.data() + i);
      scope.end = ReadBe32(table.data() + i + 4);
      scope.filter_or_handler = ReadBe32(table.data() + i + 8);
      scope.jump_target = ReadBe32(table.data() + i + 12);
      scope.opaque_roles = IsOpaqueRole(scope.filter_or_handler) ||
                           IsOpaqueRole(scope.jump_target);
      const bool valid =
          !(scope.begin & 3) && !(scope.end & 3) && scope.begin < scope.end &&
          scope.begin >= function.begin && scope.end <= function.end &&
          Containing(scope.begin, uint64_t(scope.end) - scope.begin, false,
                     true) &&
          (IsOpaqueRole(scope.filter_or_handler) ||
           (!(scope.filter_or_handler & 3) &&
            Containing(scope.filter_or_handler, 4, false, true))) &&
          (IsOpaqueRole(scope.jump_target) ||
           (!(scope.jump_target & 3) &&
            Containing(scope.jump_target, 4, false, true)));
      scope.status = valid ? ExceptionMetadataStatus::kValid
                           : ExceptionMetadataStatus::kInvalid;
      if (!valid) {
        handler.status = ExceptionMetadataStatus::kInvalid;
      }
      handler.scopes.push_back(scope);
    }
    return handler;
  }

  const ExceptionMetadataInput& input_;
  ExceptionMetadataSnapshot result_;
  uint64_t read_budget_ = 0;
  ExceptionMetadataStatus read_status_ = ExceptionMetadataStatus::kInvalid;
};

}  // namespace

ExceptionMetadataSnapshot ParseExceptionMetadata(
    const ExceptionMetadataInput& input) {
  return Parser(input).Parse();
}

ExceptionMetadataFrame ExceptionMetadataSnapshot::LookupFrame(
    uint32_t pc) const {
  ExceptionMetadataFrame frame;
  frame.pc = pc;
  if (!lookup_valid) {
    frame.status = status;
    return frame;
  }
  const auto it = std::upper_bound(functions.begin(), functions.end(), pc,
                                   [](uint32_t address, const auto& function) {
                                     return address < function.begin;
                                   });
  if (it == functions.begin()) {
    return frame;
  }
  const auto& function = *std::prev(it);
  if (!function.packed_candidate) {
    if (pc == function.begin) {
      frame.status = ExceptionMetadataStatus::kOpaque;
      frame.function = function;
    }
    return frame;
  }
  if (uint64_t(pc) >= function.end || (pc & 3)) {
    return frame;
  }
  frame.status = function.status;
  frame.function = function;
  if (function.handler &&
      function.handler->status == ExceptionMetadataStatus::kValid) {
    for (const auto& scope : function.handler->scopes) {
      if (scope.status == ExceptionMetadataStatus::kValid &&
          pc >= scope.begin && pc < scope.end) {
        frame.matching_scopes.push_back(scope);
      }
    }
  }
  return frame;
}

std::vector<ExceptionMetadataFrame> ExceptionMetadataSnapshot::DescribeFrames(
    std::span<const uint32_t> pcs) const {
  std::vector<ExceptionMetadataFrame> frames;
  frames.reserve(pcs.size());
  for (uint32_t pc : pcs) {
    frames.push_back(LookupFrame(pc));
  }
  return frames;
}

}  // namespace xe::cpu
