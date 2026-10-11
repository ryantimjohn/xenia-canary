/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_messages.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("NUI message structures have the sizes the guest expects",
          "[nui][messages]") {
  CHECK(sizeof(NuiMsg2B001) == 4);
  CHECK(sizeof(NuiMsg2B002) == 2784);
  CHECK(sizeof(NuiMsg2B003) == 24);
  CHECK(sizeof(NuiMsgIdentityIoRequest) == 24);
  CHECK(sizeof(NuiMsg2C009) == 4);
  CHECK(sizeof(NuiMsg2C00C) == 32);
  CHECK(sizeof(X_NUI_TILT_STATUS) == 0x50);
}

TEST_CASE("The HUD skeleton frame is laid out the way the title builds it",
          "[nui][messages]") {
  // The offsets the title's builder at 0x82d35958 writes: the two Vector4 at
  // +16 and +32, the six skeletons at +48, the timestamp at +2736 and the two
  // engaged tracking ids xam is allowed to answer with at +2764.
  CHECK(sizeof(X_NUI_VECTOR4) == 16);
  CHECK(sizeof(X_NUI_SKELETON_DATA) == 448);
  CHECK(sizeof(NuiMsg2B002) == 2784);

  CHECK(offsetof(NuiMsg2B002, floor_clip_plane) == 16);
  CHECK(offsetof(NuiMsg2B002, normal_to_gravity) == 32);
  CHECK(offsetof(NuiMsg2B002, skeletons) == 48);
  CHECK(offsetof(NuiMsg2B002, timestamp) == 2736);
  CHECK(offsetof(NuiMsg2B002, depth_bits) == 2748);
  CHECK(offsetof(NuiMsg2B002, engaged_tracking_id) == 2764);

  // And the fields inside one skeleton, as the publisher at 0x82d26fb8 writes
  // them and the title's own selector at 0x82a5f578 reads them back.
  CHECK(offsetof(X_NUI_SKELETON_DATA, tracking_state) == 0);
  CHECK(offsetof(X_NUI_SKELETON_DATA, tracking_id) == 4);
  CHECK(offsetof(X_NUI_SKELETON_DATA, position) == 16);
  CHECK(offsetof(X_NUI_SKELETON_DATA, skeleton_positions) == 32);
  CHECK(offsetof(X_NUI_SKELETON_DATA, position_tracking_state) == 352);
  CHECK(offsetof(X_NUI_SKELETON_DATA, quality_flags) == 432);
}

TEST_CASE("The tilt status block is laid out the way the driver reads it",
          "[nui][messages]") {
  // The NUI runtime's auto-tilt driver writes the size and the signature into
  // the block before every call (0x82d2dd28 and 0x82d2dd34) and reads three
  // accelerometer longs and a flags word back out of it (0x82d2d4c8 hands
  // +0x20..+0x2B to the runtime's three-long accelerometer helper, 0x82d2dd40
  // and 0x82d2dd44 take +0x28 and +0x24 for its atan2, 0x82d2dd90 tests +0x2C).
  CHECK(sizeof(X_NUI_TILT_STATUS) == 0x50);
  CHECK(kNuiTiltStatusSize == 0x50u);
  CHECK(kNuiTiltStatusSignature == 0x58745373u);  // 'XtSs'

  CHECK(offsetof(X_NUI_TILT_STATUS, cb_size) == 0x00);
  CHECK(offsetof(X_NUI_TILT_STATUS, signature) == 0x04);
  CHECK(offsetof(X_NUI_TILT_STATUS, accel_x) == 0x20);
  CHECK(offsetof(X_NUI_TILT_STATUS, accel_y) == 0x24);
  CHECK(offsetof(X_NUI_TILT_STATUS, accel_z) == 0x28);
  CHECK(offsetof(X_NUI_TILT_STATUS, status_flags) == 0x2C);
}

TEST_CASE("The tilt status block's tail sits where the runtime reads it",
          "[nui][messages]") {
  // The runtime reads the tail back as the floor it remembered: the plane's d
  // at +0x3C (0x82d2c71c, 0x82d2ca9c, 0x82d2ce9c, 0x82d2defc), the whole
  // plane as two doublewords from +0x30 (0x82d2d584, 0x82d2d588), a flags
  // word at +0x40 (0x82d2cd20), and the accelerometer of the moment the floor
  // was remembered from +0x44 (0x82d2c4f4 reads +0x48, 0x82d2c514 +0x4C).
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_plane) == 0x30);
  CHECK(sizeof(X_NUI_TILT_STATUS::floor_plane) == 16);
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_flags) == 0x40);
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_accel_x) == 0x44);
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_accel_y) == 0x48);
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_accel_z) == 0x4C);
  CHECK(offsetof(X_NUI_TILT_STATUS, floor_accel_z) + sizeof(int32_t) ==
        sizeof(X_NUI_TILT_STATUS));
}

TEST_CASE("NuiMsg2B002 parses the header 4D5308ED sends", "[nui][messages]") {
  // The first 32 bytes of the buffer, byte for byte as the kernel's hex dump
  // reports them in every one of the 2,992 sends of a replay run: the runtime
  // version pair and the floor clip plane the tracker measured.
  const uint8_t guest_bytes[32] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,  // version_major = 5
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2D, 0xFF,  // version_build = 11775
      0xBD, 0x0F, 0x00, 0xE9,                          // floor plane x
      0x3F, 0x7A, 0x75, 0x8B,                          // floor plane y
      0xBE, 0x50, 0xDB, 0x6B,                          // floor plane z
      0x3F, 0xBB, 0x46, 0x25,                          // floor plane w
  };

  NuiMsg2B002 message;
  std::memset(&message, 0, sizeof(message));
  std::memcpy(&message, guest_bytes, sizeof(guest_bytes));

  CHECK(message.version_major == 5);
  CHECK(message.version_build == 11775);
  CHECK(message.floor_clip_plane.x.get() == Approx(-0.034913f));
  CHECK(message.floor_clip_plane.y.get() == Approx(0.978356f));
  CHECK(message.floor_clip_plane.z.get() == Approx(-0.203962f));
  CHECK(message.floor_clip_plane.w.get() == Approx(1.463078f));

  // Nothing of the skeleton region reaches into the header the dump covers.
  CHECK(message.skeletons[0].tracking_state == 0u);
  CHECK(message.engaged_tracking_id[0] == 0u);
  CHECK(message.engaged_tracking_id[1] == 0u);
}

TEST_CASE("A HUD skeleton frame arrives without a length", "[nui][messages]") {
  // XMsgInProcessCall sends 0x2B002 with a length of zero, so the handler can
  // only reject a buffer whose length is stated and too small.
  CHECK(NuiMessageBufferFits(0, sizeof(NuiMsg2B002)));
  CHECK(NuiMessageBufferFits(2784, sizeof(NuiMsg2B002)));
  CHECK_FALSE(NuiMessageBufferFits(2736, sizeof(NuiMsg2B002)));
}

TEST_CASE("NuiMsg2B003 parses a big-endian guest buffer", "[nui][messages]") {
  // The bytes a title places in guest memory: version 5.11775 and a flags word.
  const uint8_t guest_bytes[24] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,  // version_major = 5
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2D, 0xFF,  // version_build = 11775
      0x40, 0x00, 0x02, 0x00,                          // flags = 0x40000200
      0x00, 0x00, 0x00, 0x00,                          // reserved = 0
  };

  NuiMsg2B003 message;
  std::memcpy(&message, guest_bytes, sizeof(message));

  CHECK(message.version_major == 5);
  CHECK(message.version_build == 11775);
  CHECK(message.flags == 0x40000200u);
  CHECK(message.reserved == 0u);
}

TEST_CASE("NuiMsg2B001 parses a big-endian timeout", "[nui][messages]") {
  const uint8_t guest_bytes[4] = {0x00, 0x00, 0x75, 0x30};  // 30000 ms

  NuiMsg2B001 message;
  std::memcpy(&message, guest_bytes, sizeof(message));

  CHECK(message.timeout_ms == 30000u);
}

TEST_CASE("NuiMsg2C009 parses the identity callback", "[nui][messages]") {
  const uint8_t guest_bytes[4] = {0x82, 0xD3, 0x1F, 0x18};

  NuiMsg2C009 message;
  std::memcpy(&message, guest_bytes, sizeof(message));

  CHECK(message.callback == 0x82D31F18u);
}

TEST_CASE("NuiMsg2C00C parses an identity feature registration",
          "[nui][messages]") {
  // One of the registrations 4D5308ED sends, as it sits in guest
  // memory: the feature's name lives elsewhere and is pointed at.
  const uint8_t guest_bytes[32] = {
      0x00, 0x00, 0x00, 0x02,  // version = 2
      0x40, 0x00, 0x01, 0x80,  // flags = 0x40000180
      0x00, 0x00, 0x00, 0x00,  // reserved = 0
      0x00, 0x00, 0x01, 0x01,  // unk12 = 0x0101
      0x82, 0x01, 0x85, 0xC4,  // name_ptr
      0x00, 0x00, 0x00, 0x0F,  // name_length, terminator counted in
      0x00, 0x00, 0x00, 0x00,  // unk24 = 0
      0x00, 0x00, 0x00, 0x00,  // unk28 = 0
  };

  NuiMsg2C00C message;
  std::memcpy(&message, guest_bytes, sizeof(message));

  CHECK(message.version == 2u);
  CHECK(message.flags == 0x40000180u);
  CHECK(message.unk12 == 0x0101u);
  CHECK(message.name_ptr == 0x820185C4u);
  CHECK(message.name_length == 15u);
}

TEST_CASE("NuiMsgIdentityIoRequest covers the request buffer",
          "[nui][messages]") {
  // The runtime passes a length of 0x18 with these, which is exactly the
  // buffer the handler checks for.
  CHECK(NuiMessageBufferFits(0x18, sizeof(NuiMsgIdentityIoRequest)));
  CHECK_FALSE(NuiMessageBufferFits(0x10, sizeof(NuiMsgIdentityIoRequest)));
}

TEST_CASE("NuiMessageBufferFits treats a zero length as unknown",
          "[nui][messages]") {
  // XMsgInProcessCall passes a length of zero for pointer-style messages, so a
  // zero length has to be accepted rather than rejected as too small.
  CHECK(NuiMessageBufferFits(0, sizeof(NuiMsg2B003)));
  CHECK_FALSE(NuiMessageBufferFits(4, sizeof(NuiMsg2B003)));
  CHECK(NuiMessageBufferFits(24, sizeof(NuiMsg2B003)));
  CHECK(NuiMessageBufferFits(32, sizeof(NuiMsg2B003)));
  CHECK(NuiMessageBufferFits(4, sizeof(NuiMsg2B001)));
}

TEST_CASE("xam hands the Kinect ranges and the identity requests to NUI",
          "[nui][messages]") {
  CHECK(IsNuiXamMessage(0x0002B001));
  CHECK(IsNuiXamMessage(0x0002B00C));
  CHECK(IsNuiXamMessage(0x0002C009));
  CHECK(IsNuiXamMessage(0x0002C00C));
  // The identity I/O requests the runtime starts with XMsgStartIORequest:
  // 0x2A005 and 0x2A006 while it sets itself up, then 0x2A008 and 0x2A007 for
  // each signed-in profile's biometric data.
  CHECK(IsNuiXamMessage(0x0002A005));
  CHECK(IsNuiXamMessage(0x0002A006));
  CHECK(IsNuiXamMessage(0x0002A007));
  CHECK(IsNuiXamMessage(0x0002A008));
}

TEST_CASE("xam keeps the messages NUI does not own", "[nui][messages]") {
  // XEnumerateCrossTitle is xam's own.
  CHECK_FALSE(IsNuiXamMessage(0x0002000E));
  // The runtime has wrappers for 0x2A004 and 0x2A009 too, but no run has sent
  // either, so they still fail as unhandled XAM messages, where they will be
  // noticed.
  CHECK_FALSE(IsNuiXamMessage(0x0002A004));
  CHECK_FALSE(IsNuiXamMessage(0x0002A009));
  CHECK_FALSE(IsNuiXamMessage(0x0002D000));
}

TEST_CASE("DecodeNuiFloorPlane splits the two qwords into four floats",
          "[nui][messages]") {
  // 0.05, 0.99, -0.07 and 1.31 as IEEE 754 single-precision bit patterns: the
  // shape of a plane the title's tracker fits, a near-vertical normal and an
  // offset.
  const uint64_t plane_ab = 0x3D4CCCCD3F7D70A4ull;
  const uint64_t plane_cd = 0xBD8F5C293FA7AE14ull;

  float plane[4] = {};
  DecodeNuiFloorPlane(plane_ab, plane_cd, plane);
  // The bit patterns are exactly the floats the literals round to, so the
  // comparison is exact.
  CHECK(plane[0] == 0.05f);
  CHECK(plane[1] == 0.99f);
  CHECK(plane[2] == -0.07f);
  CHECK(plane[3] == 1.31f);
}

TEST_CASE("DecodeNuiFloorPlane reads the plane as the title's ld loads it",
          "[nui][messages]") {
  // The title does `ld r3, 0(r30); ld r4, 8(r30)` on a plane held as four
  // big-endian floats in guest memory, so the register the export sees holds
  // the first float in its high half and the second in its low half. Here the
  // same sixteen bytes are read back as the two big-endian qwords the export
  // arguments are.
  const uint8_t guest_bytes[16] = {
      0x3D, 0x4C, 0xCC, 0xCD,  // a = 0.05
      0x3F, 0x7D, 0x70, 0xA4,  // b = 0.99
      0xBD, 0x8F, 0x5C, 0x29,  // c = -0.07
      0x3F, 0xA7, 0xAE, 0x14,  // d = 1.31
  };
  xe::be<uint64_t> words[2];
  std::memcpy(words, guest_bytes, sizeof(words));

  float plane[4] = {};
  DecodeNuiFloorPlane(words[0], words[1], plane);
  CHECK(plane[0] == 0.05f);
  CHECK(plane[1] == 0.99f);
  CHECK(plane[2] == -0.07f);
  CHECK(plane[3] == 1.31f);
}

TEST_CASE("DecodeNuiFloorPlane keeps a zero plane and the sign of a zero",
          "[nui][messages]") {
  // Before the tracker has fitted a floor the title hands over zeros, and a
  // negative zero must not turn into a positive one on the way.
  float plane[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  DecodeNuiFloorPlane(0x0000000080000000ull, 0, plane);
  CHECK(plane[0] == 0.0f);
  CHECK_FALSE(std::signbit(plane[0]));
  CHECK(plane[1] == 0.0f);
  CHECK(std::signbit(plane[1]));
  CHECK(plane[2] == 0.0f);
  CHECK(plane[3] == 0.0f);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
