/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GUEST_RANGE_H_
#define XENIA_KERNEL_UTIL_GUEST_RANGE_H_

#include <cstdint>

#include "xenia/base/memory.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace util {

// Whether the guest can really read `length` bytes at `address`, and whether it
// can also write them. A request or a message arrives with pointers the title
// chose, and an address inside a heap can still sit on pages that are not
// committed or not readable, so a handler asks before it reads or writes
// through one and refuses the request when the answer is no. A null address, an
// empty range, an address outside every heap and a range that leaves its heap
// or wraps are all refused.
inline bool IsGuestRangeReadable(Memory* memory, uint32_t address,
                                 uint32_t length) {
  if (!address || !length) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  return heap->QueryRangeAccess(address, address + length - 1) !=
         xe::memory::PageAccess::kNoAccess;
}

inline bool IsGuestRangeWritable(Memory* memory, uint32_t address,
                                 uint32_t length) {
  if (!address || !length) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  const auto access = heap->QueryRangeAccess(address, address + length - 1);
  return access == xe::memory::PageAccess::kReadWrite ||
         access == xe::memory::PageAccess::kExecuteReadWrite;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GUEST_RANGE_H_
