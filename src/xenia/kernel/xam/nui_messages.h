/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NUI_MESSAGES_H_
#define XENIA_KERNEL_XAM_NUI_MESSAGES_H_

#include <bit>
#include <cstddef>
#include <cstdint>

#include "xenia/base/byte_order.h"

namespace xe {
namespace kernel {
namespace xam {

// Payloads of the app 0xFE messages in the 0x0002Bxxx range, as they are laid
// out in guest memory. Every field is big endian, the way the title wrote it.

// 0x0002B001: asks xam to keep the camera awake for the given time. A timeout
// of zero releases the request.
struct NuiMsg2B001 {
  xe::be<uint32_t> timeout_ms;
};
static_assert(sizeof(NuiMsg2B001) == 4, "Size matters");

// The number of skeleton slots a NUI frame carries. At most two of them are
// ever tracked at once, but the array is always six long.
constexpr size_t kNuiSkeletonCount = 6;

// The number of joints a tracked NUI skeleton has.
constexpr size_t kNuiSkeletonPositionCount = 20;

// A point or a plane, as the NUI runtime's own math lays it out.
struct X_NUI_VECTOR4 {
  xe::be<float> x;
  xe::be<float> y;
  xe::be<float> z;
  xe::be<float> w;
};
static_assert(sizeof(X_NUI_VECTOR4) == 16, "Size matters");

// One skeleton slot of a NUI frame, as the title's own tracker publishes it
// (0x82d26fb8) and its engagement selector reads it back (0x82a5f578).
struct X_NUI_SKELETON_DATA {
  // NUI_SKELETON_NOT_TRACKED 0, NUI_SKELETON_POSITION_ONLY 1,
  // NUI_SKELETON_TRACKED 2. A title looks for 2 before it will treat the slot
  // as a player.
  xe::be<uint32_t> tracking_state;  // +0
  xe::be<uint32_t> tracking_id;     // +4
  // The publisher memsets the whole array and then writes only the fields
  // around these two, so for this runtime version they are always zero: they
  // are not a channel xam fills in.
  xe::be<uint32_t> enrollment_index;  // +8
  xe::be<uint32_t> user_index;        // +12
  // Where the skeleton is, in millimetres from the sensor.
  X_NUI_VECTOR4 position;                                       // +16
  X_NUI_VECTOR4 skeleton_positions[kNuiSkeletonPositionCount];  // +32
  // One NUI_SKELETON_POSITION_TRACKING_STATE per joint: not tracked,
  // inferred or tracked.
  xe::be<uint32_t> position_tracking_state[kNuiSkeletonPositionCount];  // +352
  xe::be<uint32_t> quality_flags;                                       // +432
  xe::be<uint32_t> reserved[3];                                         // +436
};
static_assert(sizeof(X_NUI_SKELETON_DATA) == 448, "Size matters");
static_assert(offsetof(X_NUI_SKELETON_DATA, tracking_state) == 0,
              "tracking_state is the first word of a skeleton slot");
static_assert(offsetof(X_NUI_SKELETON_DATA, tracking_id) == 4,
              "tracking_id follows tracking_state");
static_assert(offsetof(X_NUI_SKELETON_DATA, position) == 16,
              "position sits after the four index words");
static_assert(offsetof(X_NUI_SKELETON_DATA, skeleton_positions) == 32,
              "the joint positions follow the skeleton's own position");
static_assert(offsetof(X_NUI_SKELETON_DATA, position_tracking_state) == 352,
              "the per-joint tracking states follow the twenty positions");
static_assert(offsetof(X_NUI_SKELETON_DATA, quality_flags) == 432,
              "quality_flags follows the per-joint tracking states");

// 0x0002B002: the HUD skeleton feed. The title hands xam the skeleton frame it
// has just processed together with the two locked HUD textures a system
// silhouette would be drawn from, once per processed frame. The buffer is
// built at 0x82d35958 and sent at 0x82d35a40; only the two engaged tracking
// ids are ever read back out of it, and only when xam answers 0x8301000B.
struct NuiMsg2B002 {
  xe::be<uint64_t> version_major;                    // +0, 5 in 4D5308ED
  xe::be<uint64_t> version_build;                    // +8, 11775
  X_NUI_VECTOR4 floor_clip_plane;                    // +16
  X_NUI_VECTOR4 normal_to_gravity;                   // +32
  X_NUI_SKELETON_DATA skeletons[kNuiSkeletonCount];  // +48
  xe::be<uint64_t> timestamp;                        // +2736
  xe::be<uint32_t> reserved0;                        // +2744
  // The two textures, as D3DLOCKED_RECT pairs of a guest pointer and a pitch.
  xe::be<uint32_t> depth_bits;              // +2748
  xe::be<uint32_t> depth_pitch;             // +2752
  xe::be<uint32_t> player_index_bits;       // +2756
  xe::be<uint32_t> player_index_pitch;      // +2760
  xe::be<uint32_t> engaged_tracking_id[2];  // +2764, [out]
  xe::be<uint32_t> reserved1[3];            // +2772
};
static_assert(sizeof(NuiMsg2B002) == 2784, "Size matters");
static_assert(offsetof(NuiMsg2B002, floor_clip_plane) == 16,
              "the floor plane follows the two version words");
static_assert(offsetof(NuiMsg2B002, normal_to_gravity) == 32,
              "the gravity normal follows the floor plane");
static_assert(offsetof(NuiMsg2B002, skeletons) == 48,
              "the six skeleton slots follow the two planes");
static_assert(offsetof(NuiMsg2B002, timestamp) == 2736,
              "the timestamp follows the six skeleton slots");
static_assert(offsetof(NuiMsg2B002, depth_bits) == 2748,
              "the depth texture's locked rect follows the timestamp");
static_assert(offsetof(NuiMsg2B002, depth_pitch) == 2752,
              "the depth texture's pitch follows its bits pointer");
static_assert(offsetof(NuiMsg2B002, player_index_bits) == 2756,
              "the player-index texture's locked rect follows the depth one");
static_assert(offsetof(NuiMsg2B002, player_index_pitch) == 2760,
              "the player-index texture's pitch follows its bits pointer");
static_assert(offsetof(NuiMsg2B002, engaged_tracking_id) == 2764,
              "the engaged ids follow the two locked rects");

// 0x0002B003: hands xam the NUI runtime version and the HUD initialization
// flags. 4D5308ED sends {5, 11775, flags, 0}.
struct NuiMsg2B003 {
  xe::be<uint64_t> version_major;
  xe::be<uint64_t> version_build;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> reserved;
};
static_assert(sizeof(NuiMsg2B003) == 24, "Size matters");

// The block XamNuiCameraTiltGetStatus fills. It is not a message, but it is
// the same kind of thing: a piece of guest memory whose layout is only known
// from what the NUI runtime writes into it and reads back out.
constexpr uint32_t kNuiTiltStatusSize = 0x50;

// 'XtSs', the tag the caller stamps the block with before the call
// (0x82d2dd24 builds it, 0x82d2dd34 stores it).
constexpr uint32_t kNuiTiltStatusSignature = 0x58745373;

// The tilt-status block, as the runtime's auto-tilt driver at 0x82d2dd3c uses
// it. The caller fills in the first two fields; xam writes the whole 0x50
// bytes back and re-stamps those two with the same constants, so the caller
// sees the values it wrote (FillNuiTiltStatusLegs in nui_tilt_status.h). The
// runtime reads everything from +0x20 on back out: the three accelerometer
// longs and the flags word say where the sensor is now, and the tail from
// +0x30 hands back the floor the title last gave XamNuiCameraRememberFloor,
// with the accelerometer as it was when it did (FillNuiTiltStatusFloor). The
// field names are the runtime's own reading of them: it takes
// atan2(accel_z, accel_y) for the elevation, so +0x24 is the leg that carries
// the cosine of the tilt and +0x28 the leg that carries the sine. No console
// has been seen to fill the tail; its layout is what the runtime's dozen and
// more reads of it agree on.
struct X_NUI_TILT_STATUS {
  xe::be<uint32_t> cb_size;    // +0x00, 0x50
  xe::be<uint32_t> signature;  // +0x04, 'XtSs'
  uint8_t reserved0[0x18];     // +0x08
  // The three accelerometer axes the runtime's public accessor hands on as a
  // float4 (0x82d2d4c8 passes +0x20 to 0x82d2c938).
  xe::be<int32_t> accel_x;        // +0x20
  xe::be<int32_t> accel_y;        // +0x24
  xe::be<int32_t> accel_z;        // +0x28
  xe::be<uint32_t> status_flags;  // +0x2C
  // The remembered floor, exactly as XamNuiCameraRememberFloor received it:
  // the unit normal's a, b and c, then d, the sensor's height above the floor
  // in millimetres. The runtime's tilt target (0x82d2c71c), its search
  // (0x82d2ca9c), its start (0x82d2ce9c) and its poll (0x82d2defc) all branch
  // on d being zero, which means "no floor"; the frame path publishes the
  // plane as the frame's floor when the tracker sees none (0x82d2d584), and
  // the framing hands it to the player-extents routine 0x82d49940.
  xe::be<float> floor_plane[4];  // +0x30
  // Bit 0 makes the runtime's tilt call give up with 5 before it does
  // anything (0x82d2cd20), presumably "auto-tilt is off". xenia writes 0.
  xe::be<uint32_t> floor_flags;  // +0x40
  // The accelerometer of the moment the floor was remembered, in the encoding
  // and scale of +0x20..+0x28: 0x82d2c4f4 and 0x82d2c514 take atan2(+0x4C,
  // +0x48), and 0x82d2d5ac, 0x82d2e180 and 0x82d2e624 pass all three on by
  // address.
  xe::be<int32_t> floor_accel_x;  // +0x44
  xe::be<int32_t> floor_accel_y;  // +0x48
  xe::be<int32_t> floor_accel_z;  // +0x4C
};
static_assert(sizeof(X_NUI_TILT_STATUS) == 0x50, "Size matters");
static_assert(offsetof(X_NUI_TILT_STATUS, cb_size) == 0x00,
              "cb_size is the first word the caller stamps");
static_assert(offsetof(X_NUI_TILT_STATUS, signature) == 0x04,
              "the signature follows cb_size");
static_assert(offsetof(X_NUI_TILT_STATUS, accel_x) == 0x20,
              "the accelerometer starts after the first reserved region");
static_assert(offsetof(X_NUI_TILT_STATUS, accel_y) == 0x24,
              "accel_y carries the cosine leg of the tilt");
static_assert(offsetof(X_NUI_TILT_STATUS, accel_z) == 0x28,
              "accel_z carries the sine leg of the tilt");
static_assert(offsetof(X_NUI_TILT_STATUS, status_flags) == 0x2C,
              "the flags word follows the three accelerometer legs");
static_assert(offsetof(X_NUI_TILT_STATUS, floor_plane) == 0x30,
              "the remembered floor plane follows the flags word");
static_assert(sizeof(X_NUI_TILT_STATUS::floor_plane) == 0x10,
              "the remembered floor plane is four floats");
static_assert(offsetof(X_NUI_TILT_STATUS, floor_flags) == 0x40,
              "the floor's flags word follows the plane");
static_assert(offsetof(X_NUI_TILT_STATUS, floor_accel_x) == 0x44,
              "the remembered accelerometer follows the floor's flags word");
static_assert(offsetof(X_NUI_TILT_STATUS, floor_accel_y) == 0x48,
              "floor_accel_y carries the remembered cosine leg");
static_assert(offsetof(X_NUI_TILT_STATUS, floor_accel_z) == 0x4C,
              "floor_accel_z carries the remembered sine leg and ends the "
              "block");

// Payloads of the identity messages, which the NUI runtime sends while it
// brings up player recognition.

// 0x0002A005 to 0x0002A008: started with XMsgStartIORequest and no overlapped,
// 0x2A005 and 0x2A006 during identity initialization and 0x2A008 and 0x2A007
// for each profile whose biometric data the runtime brings up to date. What
// xam does with them is not known; the layout is only what the runtime fills
// in, so that the handler can check the buffer. 0x2A007 and 0x2A008 store the
// profile's XUID where unk0 and unk4 are.
struct NuiMsgIdentityIoRequest {
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk4;
  xe::be<uint64_t> unk8;
  xe::be<uint32_t> unk16;
  xe::be<uint32_t> unk20;
};
static_assert(sizeof(NuiMsgIdentityIoRequest) == 24, "Size matters");

// 0x0002C009: registers the routine xam calls when an identity event happens.
// The message is the last step of the identity initialization and its result
// is that initialization's result.
struct NuiMsg2C009 {
  xe::be<uint32_t> callback;
};
static_assert(sizeof(NuiMsg2C009) == 4, "Size matters");

// 0x0002C00C: registers one identity feature. 4D5308ED sends about
// twenty of these, with names such as NuiV1FaceHiRes held in title memory.
struct NuiMsg2C00C {
  xe::be<uint32_t> version;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> reserved;
  xe::be<uint32_t> unk12;
  xe::be<uint32_t> name_ptr;
  xe::be<uint32_t> name_length;
  xe::be<uint32_t> unk24;
  xe::be<uint32_t> unk28;
};
static_assert(sizeof(NuiMsg2C00C) == 32, "Size matters");

// The floor plane the title hands XamNuiCameraRememberFloor. The title builds
// the call at 0x82d2cc1c and 0x82d2cc84 with `ld r3, 0(r30); ld r4, 8(r30)`:
// its plane is four big-endian floats in guest memory, in the order of the
// HUD skeleton frame's floor_clip_plane (the three components of the unit
// normal, then the sensor's distance from the floor), and each ld loads two of
// them into one register with the earlier float in the high half. plane_ab and
// plane_cd are those two registers as the export receives them, so a is the
// high half of the first, b its low half, c the high half of the second and d
// its low half. The floats are decoded as they are: d arrives in millimetres,
// where the HUD frame's own plane carries metres (the runtime converts it for
// the frame), so a replay's 1262.906 here is the 1.262906 there.
inline void DecodeNuiFloorPlane(uint64_t plane_ab, uint64_t plane_cd,
                                float out[4]) {
  out[0] = std::bit_cast<float>(static_cast<uint32_t>(plane_ab >> 32));
  out[1] = std::bit_cast<float>(static_cast<uint32_t>(plane_ab));
  out[2] = std::bit_cast<float>(static_cast<uint32_t>(plane_cd >> 32));
  out[3] = std::bit_cast<float>(static_cast<uint32_t>(plane_cd));
}

// Returns true when a message buffer of the given length is large enough to
// hold a payload of the given size. XMsgInProcessCall passes a length of zero
// for pointer-style messages, so zero means "unknown size" rather than "empty".
inline bool NuiMessageBufferFits(uint32_t buffer_length, size_t needed) {
  return buffer_length == 0 || buffer_length >= needed;
}

// Returns true for the app 0xFE messages xam hands to the NUI state rather
// than to its own handlers: the two Kinect ranges, 0x0002Bxxx and 0x0002Cxxx,
// and the identity I/O requests 0x0002A005 to 0x0002A008, which the NUI
// runtime starts with XMsgStartIORequest outside them.
constexpr bool IsNuiXamMessage(uint32_t message) {
  return (message & 0xFFFFF000) == 0x0002B000 ||
         (message & 0xFFFFF000) == 0x0002C000 ||
         (message >= 0x0002A005 && message <= 0x0002A008);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NUI_MESSAGES_H_
