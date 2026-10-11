/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XLIVEBASE_MESSAGES_H_
#define XENIA_KERNEL_XAM_APPS_XLIVEBASE_MESSAGES_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "xenia/base/byte_order.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

// Payloads of the app 0xFC (XLIVEBASE) messages, as they are laid out in
// guest memory. Every field is big endian, the way the title wrote it.

// 0x0005800E: XUserMuteListQuery(user_index, remote_xuid, on_mute_list). The
// title's XLive library puts the first two arguments in this buffer, passes
// the address of the BOOL as the message's second argument, and returns what
// xam leaves in `result` as the call's own result.
struct X_USER_MUTE_LIST_QUERY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> pad4;
  xe::be<uint64_t> remote_xuid;
  xe::be<uint32_t> result;
  xe::be<uint32_t> pad20;
};
static_assert(sizeof(X_USER_MUTE_LIST_QUERY) == 24, "Size matters");
static_assert(offsetof(X_USER_MUTE_LIST_QUERY, remote_xuid) == 8,
              "the XUID is stored with std at +8");
static_assert(offsetof(X_USER_MUTE_LIST_QUERY, result) == 16,
              "the result is read back from +16");

// What xam answers XUserMuteListQuery with, decided from the asker's user
// index alone so that the decision can be tested without a guest.
struct UserMuteListAnswer {
  // The status the title's XLive library hands back as the call's result.
  uint32_t result = X_ERROR_SUCCESS;
  // The BOOL to store through the message's second argument, or nothing when
  // the query is refused and the caller's BOOL stays as it was.
  std::optional<uint32_t> on_mute_list;
};

// Xenia keeps no mute list, so nobody is on it: a user who is signed in gets
// ERROR_SUCCESS and a BOOL of FALSE. An index that is not one of the four
// users gets ERROR_INVALID_PARAMETER and one that is not signed in gets
// ERROR_NO_SUCH_USER, as xam_user.cc answers them, and both leave the BOOL
// alone. Whether a user is signed in is asked only for an index that is one of
// the four, because XamState::IsUserSignedIn cuts the index to a byte and reads
// 0xFD, 0xFE and 0xFF as the latest, no and any user: asked first, it would let
// 0x100 stand for user 0 and 0xFF for whoever is signed in.
template <typename SignedInFn>
UserMuteListAnswer AnswerUserMuteListQuery(uint32_t user_index,
                                           SignedInFn&& is_signed_in) {
  UserMuteListAnswer answer;
  if (user_index >= XUserMaxUserCount) {
    answer.result = X_ERROR_INVALID_PARAMETER;
  } else if (!is_signed_in(user_index)) {
    answer.result = X_ERROR_NO_SUCH_USER;
  } else {
    answer.on_mute_list = 0;
  }
  return answer;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XLIVEBASE_MESSAGES_H_
