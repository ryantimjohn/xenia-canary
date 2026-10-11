/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
namespace test {

TEST_CASE("The mute list query is laid out the way the title builds it",
          "[xam][xlivebase]") {
  // 4D5308ED' XUserMuteListQuery (0x82d8f660) stores the user index
  // with stw at +0, the XUID with std at +8 and a zero with stw at +16, and
  // returns whatever is at +16 after the call.
  CHECK(sizeof(X_USER_MUTE_LIST_QUERY) == 24);
  CHECK(offsetof(X_USER_MUTE_LIST_QUERY, user_index) == 0);
  CHECK(offsetof(X_USER_MUTE_LIST_QUERY, remote_xuid) == 8);
  CHECK(offsetof(X_USER_MUTE_LIST_QUERY, result) == 16);
}

TEST_CASE("The mute list query parses a big-endian guest buffer",
          "[xam][xlivebase]") {
  const uint8_t guest[24] = {
      0x00, 0x00, 0x00, 0x02,                          // user index 2
      0x00, 0x00, 0x00, 0x00,                          // padding
      0xE0, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,  // remote XUID
      0x00, 0x00, 0x00, 0x00,                          // result, zeroed
      0x00, 0x00, 0x00, 0x00,                          // padding
  };
  X_USER_MUTE_LIST_QUERY query;
  std::memcpy(&query, guest, sizeof(query));
  CHECK(query.user_index == 2u);
  CHECK(query.remote_xuid == 0xE0000123456789ABull);
  CHECK(query.result == 0u);

  // The answer goes back the same way round.
  query.result = 0x525;
  uint8_t answered[24];
  std::memcpy(answered, &query, sizeof(answered));
  CHECK(answered[16] == 0x00);
  CHECK(answered[17] == 0x00);
  CHECK(answered[18] == 0x05);
  CHECK(answered[19] == 0x25);
}

TEST_CASE("A signed-in user is on nobody's mute list", "[xam][xlivebase]") {
  // Xenia keeps no mute list, so the query succeeds and the BOOL it writes is
  // FALSE, for each of the four users, after asking about that user alone.
  for (uint32_t user_index = 0; user_index < 4; ++user_index) {
    INFO("user index " << user_index);
    int asked = 0;
    uint32_t asked_about = 0xFFFFFFFFu;
    const UserMuteListAnswer answer =
        AnswerUserMuteListQuery(user_index, [&](uint32_t index) {
          ++asked;
          asked_about = index;
          return true;
        });
    CHECK(answer.result == 0u);  // ERROR_SUCCESS
    REQUIRE(answer.on_mute_list.has_value());
    CHECK(*answer.on_mute_list == 0u);
    CHECK(asked == 1);
    CHECK(asked_about == user_index);
  }
}

TEST_CASE("A user index past the four users is refused unasked",
          "[xam][xlivebase]") {
  // ERROR_INVALID_PARAMETER, and the BOOL is left alone, whether or not the
  // user would be signed in. Whether the user is signed in is not even asked:
  // XamState::IsUserSignedIn cuts the index to a byte and reads 0xFD, 0xFE and
  // 0xFF as the latest, no and any user, so asking first would let 0x100 stand
  // for user 0 and 0xFF for whoever is signed in.
  for (uint32_t user_index :
       {4u, 5u, 0xFDu, 0xFEu, 0xFFu, 0x100u, 0xFFFFFFFFu}) {
    for (bool signed_in : {false, true}) {
      INFO("user index " << user_index << ", signed in " << signed_in);
      int asked = 0;
      const UserMuteListAnswer answer =
          AnswerUserMuteListQuery(user_index, [&](uint32_t) {
            ++asked;
            return signed_in;
          });
      CHECK(answer.result == 0x57u);  // ERROR_INVALID_PARAMETER
      CHECK_FALSE(answer.on_mute_list.has_value());
      CHECK(asked == 0);
    }
  }
}

TEST_CASE("A user who is not signed in is refused as no such user",
          "[xam][xlivebase]") {
  // ERROR_NO_SUCH_USER, and the BOOL is left alone.
  for (uint32_t user_index = 0; user_index < 4; ++user_index) {
    INFO("user index " << user_index);
    const UserMuteListAnswer answer =
        AnswerUserMuteListQuery(user_index, [](uint32_t) { return false; });
    CHECK(answer.result == 0x525u);  // ERROR_NO_SUCH_USER
    CHECK_FALSE(answer.on_mute_list.has_value());
  }

  // Each user is judged on their own: with only user 2 signed in, user 1 is
  // refused and user 2 is not.
  auto only_user_2 = [](uint32_t index) { return index == 2; };
  CHECK(AnswerUserMuteListQuery(1, only_user_2).result == 0x525u);
  CHECK(AnswerUserMuteListQuery(2, only_user_2).result == 0u);
}

}  // namespace test
}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
