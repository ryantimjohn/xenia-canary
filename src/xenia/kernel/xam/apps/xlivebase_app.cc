/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/util/guest_range.h"
#include "xenia/kernel/xam/apps/xlivebase_messages.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      return 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x0005800E: {
      // XUserMuteListQuery. The second argument is the address of the
      // caller's BOOL, not a length.
      return UserMuteListQuery(buffer_ptr, buffer_length);
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      XELOGD("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

X_HRESULT XLiveBaseApp::UserMuteListQuery(uint32_t buffer_ptr,
                                          uint32_t on_mute_list_ptr) {
  // Xenia keeps no mute list, so nobody is on it. 4D5308ED' voice
  // code asks about each talker in its table for each signed-in user, many
  // times a second for as long as a profile is signed in, and reads only the
  // BOOL: FALSE leaves the talker audible. Its XLive library drops the
  // message's own result and returns the buffer's, which none of its three
  // callers looks at.
  if (!util::IsGuestRangeWritable(memory_, buffer_ptr,
                                  sizeof(X_USER_MUTE_LIST_QUERY)) ||
      !util::IsGuestRangeWritable(memory_, on_mute_list_ptr,
                                  sizeof(uint32_t))) {
    XELOGD("XUserMuteListQuery({:08X}, {:08X}): not a writable request",
           buffer_ptr, on_mute_list_ptr);
    return X_E_INVALIDARG;
  }
  auto* query = memory_->TranslateVirtual<X_USER_MUTE_LIST_QUERY*>(buffer_ptr);
  const uint32_t user_index = query->user_index;
  const UserMuteListAnswer answer =
      AnswerUserMuteListQuery(user_index, [this](uint32_t index) {
        return kernel_state_->xam_state()->IsUserSignedIn(index);
      });
  if (answer.on_mute_list) {
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(on_mute_list_ptr),
                                 *answer.on_mute_list);
  }
  query->result = answer.result;
  XELOGD("XUserMuteListQuery({}, {:016X}, {:08X}) = {:08X}", user_index,
         query->remote_xuid.get(), on_mute_list_ptr, answer.result);
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
