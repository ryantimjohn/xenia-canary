/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/nui_tilt_status.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

namespace {

// What the NUI runtime's auto-tilt driver makes of the block, instruction for
// instruction: atan2 of the two accelerometer legs (0x82d2dd40 reads +0x28 and
// 0x82d2dd44 reads +0x24, 0x82d2dd6c calls atan2) scaled by the
// 57.2957763671875 it keeps at 0x82018068. The driver stops moving the motor
// when this is within two degrees of the angle it asked for.
double DriverMeasuredAngle(const X_NUI_TILT_STATUS& block) {
  return std::atan2(static_cast<double>(block.accel_z.get()),
                    static_cast<double>(block.accel_y.get())) *
         (180.0 / 3.14159265358979323846);
}

// What the host runtime's accelerometer reads for a head pitched up by the
// given angle: a vector toward the floor, in g, with y up and z the way the
// sensor looks, so a level sensor reads (0, -1, 0).
void ReadingForElevation(double degrees, float* xyz) {
  const double radians = degrees * (3.14159265358979323846 / 180.0);
  xyz[0] = 0.0f;
  xyz[1] = static_cast<float>(-std::cos(radians));
  xyz[2] = static_cast<float>(-std::sin(radians));
}

// How many bytes of the block's reserved region and of its floor tail are not
// zero. A fill clears both, and the tail stays clear until a remembered floor
// is written into it.
uint32_t ReservedBytesSet(const X_NUI_TILT_STATUS& block) {
  uint8_t bytes[sizeof(X_NUI_TILT_STATUS)];
  std::memcpy(bytes, &block, sizeof(bytes));
  uint32_t set = 0;
  for (size_t i = 0x08; i < 0x20; ++i) {
    set += bytes[i] ? 1u : 0u;
  }
  for (size_t i = 0x30; i < 0x50; ++i) {
    set += bytes[i] ? 1u : 0u;
  }
  return set;
}

// The elevation the runtime takes from the tail's accelerometer, the one of
// the moment the floor was remembered: atan2(+0x4C, +0x48) (0x82d2c4f4 and
// 0x82d2c514 read the two legs), in degrees as DriverMeasuredAngle has it.
double DriverRememberedAngle(const X_NUI_TILT_STATUS& block) {
  return std::atan2(static_cast<double>(block.floor_accel_z.get()),
                    static_cast<double>(block.floor_accel_y.get())) *
         (180.0 / 3.14159265358979323846);
}

// The tilt the runtime aims for once the block hands it a floor, its target
// routine's floor branch (0x82d2c6f8, taken when +0x3C is not zero at
// 0x82d2c71c): put the top of a 22.3654-degree half view (0x82018064) on
// the head of a 1800 mm player (0x82018070) standing 2200 mm away (the
// 1/2200 at 0x8201806c), with margin_mm to spare above the head, for a
// sensor d millimetres above the floor. The result is rounded down after
// adding the 0.5 at 0x82001178 and clamped to the motor's 27 degrees.
int32_t DriverFloorTarget(const X_NUI_TILT_STATUS& block, double margin_mm) {
  const double d = block.floor_plane[3].get();
  const double degrees =
      std::atan((margin_mm + 1800.0 - d) / 2200.0) * 57.2957763671875;
  const auto target = static_cast<int32_t>(std::floor(degrees - 22.3654 + 0.5));
  return target > 27 ? 27 : (target < -27 ? -27 : target);
}

// The block XamNuiCameraTiltGetStatus hands back is filled from accelerometer
// legs that NuiState::GetTiltStatusLegs makes from the sensor's reading when
// it has one and from the angle otherwise. These make the same two kinds of
// legs and fill a block from them; moving says whether the motor is on its way.
void FillForAngle(X_NUI_TILT_STATUS* block, int32_t degrees,
                  bool moving = false) {
  FillNuiTiltStatusLegs(block, NuiTiltStatusLegsForAngle(degrees), moving);
}

void FillFromGravity(X_NUI_TILT_STATUS* block, const float* xyz,
                     bool moving = false) {
  FillNuiTiltStatusLegs(block, NuiTiltStatusLegsFromGravity(xyz), moving);
}

// A floor the title's tracker could have fitted: a near-vertical unit normal
// and the sensor d millimetres above the floor.
std::array<float, 4> FloorPlane(float d) { return {0.05f, 0.99f, -0.07f, d}; }

// A floor remembered with the plane given, while the block's accelerometer
// described a sensor at the given angle.
NuiRememberedFloor RememberedFloorAt(const std::array<float, 4>& plane,
                                     int32_t degrees) {
  NuiRememberedFloor floor;
  floor.plane = plane;
  floor.legs = NuiTiltStatusLegsForAngle(degrees);
  return floor;
}

}  // namespace

TEST_CASE("A filled tilt status block reads back as the angle it was given",
          "[nui][tilt]") {
  // The whole point of the block: every angle a Kinect can hold has to come
  // back out of the driver's atan2 as itself, comfortably inside the two
  // degrees its convergence test allows (0x82001168 = 2.0, tested at
  // 0x82d2df7c and 0x82d2e070). Half a degree is the bar here, and with a
  // gravity of 981 the rounding never costs a tenth of one.
  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    X_NUI_TILT_STATUS block;
    std::memset(&block, 0xCD, sizeof(block));
    FillForAngle(&block, degrees);

    REQUIRE(std::fabs(DriverMeasuredAngle(block) - degrees) < 0.5);
    REQUIRE(block.cb_size.get() == kNuiTiltStatusSize);
    REQUIRE(block.signature.get() == kNuiTiltStatusSignature);
    REQUIRE(block.status_flags.get() == 0u);
    REQUIRE(block.accel_x.get() == 0);
  }
}

TEST_CASE("A level sensor puts the whole of gravity on the cosine leg",
          "[nui][tilt]") {
  X_NUI_TILT_STATUS block;
  std::memset(&block, 0xCD, sizeof(block));
  FillForAngle(&block, 0);

  CHECK(kNuiTiltGravity == 981);
  CHECK(block.accel_x.get() == 0);
  CHECK(block.accel_y.get() == 981);
  CHECK(block.accel_z.get() == 0);
  CHECK(DriverMeasuredAngle(block) == Approx(0.0));
}

TEST_CASE("Tilting the sensor moves the sine leg the way the driver expects",
          "[nui][tilt]") {
  // Only the ratio of the two legs matters, but the sign of the sine leg has
  // to follow the angle or the driver would settle at the wrong end of its
  // travel.
  X_NUI_TILT_STATUS up;
  X_NUI_TILT_STATUS down;
  FillForAngle(&up, 27);
  FillForAngle(&down, -27);

  CHECK(up.accel_z.get() > 0);
  CHECK(down.accel_z.get() < 0);
  CHECK(up.accel_y.get() > 0);
  CHECK(down.accel_y.get() > 0);
  CHECK(up.accel_y.get() == down.accel_y.get());
  CHECK(up.accel_z.get() == -down.accel_z.get());
}

TEST_CASE("The tilt status block reaches the guest big-endian", "[nui][tilt]") {
  // The title reads the block out of its own memory, so every field has to sit
  // in guest byte order, and the two reserved regions have to be clean: the
  // block is a stack buffer the title only partly initializes.
  X_NUI_TILT_STATUS block;
  std::memset(&block, 0xCD, sizeof(block));
  FillForAngle(&block, 0);

  uint8_t bytes[sizeof(X_NUI_TILT_STATUS)];
  std::memcpy(bytes, &block, sizeof(bytes));

  // cb_size = 0x50 and the signature the title itself wrote in, kept rather
  // than wiped.
  CHECK(bytes[0x00] == 0x00);
  CHECK(bytes[0x01] == 0x00);
  CHECK(bytes[0x02] == 0x00);
  CHECK(bytes[0x03] == 0x50);
  CHECK(bytes[0x04] == 0x58);  // 'X'
  CHECK(bytes[0x05] == 0x74);  // 't'
  CHECK(bytes[0x06] == 0x53);  // 'S'
  CHECK(bytes[0x07] == 0x73);  // 's'

  // accel_y = 981 = 0x000003D5, most significant byte first.
  CHECK(bytes[0x24] == 0x00);
  CHECK(bytes[0x25] == 0x00);
  CHECK(bytes[0x26] == 0x03);
  CHECK(bytes[0x27] == 0xD5);

  CHECK(ReservedBytesSet(block) == 0u);
}

TEST_CASE("A block filled from a measured reading reads back as its elevation",
          "[nui][tilt]") {
  // The block and the angle XamNuiCameraElevationGetAngle reports have to
  // tell the title the same thing, or its auto-tilt driver would keep
  // commanding the motor toward a target it believes it never reaches.
  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    float reading[3];
    ReadingForElevation(degrees, reading);
    X_NUI_TILT_STATUS block;
    std::memset(&block, 0xCD, sizeof(block));
    FillFromGravity(&block, reading);

    REQUIRE(std::fabs(DriverMeasuredAngle(block) - degrees) < 0.5);
    REQUIRE(std::fabs(DriverMeasuredAngle(block) -
                      xe::nui::AccelerometerElevationDegrees(reading)) < 0.1);
    REQUIRE(block.cb_size.get() == kNuiTiltStatusSize);
    REQUIRE(block.signature.get() == kNuiTiltStatusSignature);
    REQUIRE(block.status_flags.get() == 0u);
    REQUIRE(block.accel_x.get() == 0);
    REQUIRE(ReservedBytesSet(block) == 0u);
  }
}

TEST_CASE("A level reading fills the block exactly as a level angle does",
          "[nui][tilt]") {
  const float level[3] = {0.0f, -1.0f, 0.0f};
  X_NUI_TILT_STATUS measured;
  X_NUI_TILT_STATUS synthesised;
  std::memset(&measured, 0xCD, sizeof(measured));
  std::memset(&synthesised, 0x5A, sizeof(synthesised));
  FillFromGravity(&measured, level);
  FillForAngle(&synthesised, 0);

  CHECK(measured.accel_y.get() == kNuiTiltGravity);
  CHECK(measured.accel_z.get() == 0);
  CHECK(std::memcmp(&measured, &synthesised, sizeof(measured)) == 0);
}

TEST_CASE("The block follows where the sensor is, not where it was asked to go",
          "[nui][tilt]") {
  // The title asked for +27 degrees and the sensor sat at about 8.5 degrees
  // down. A block made from the requested angle would say +27; made from the
  // reading, the block says where the sensor really points, to a fraction of a
  // degree.
  float reading[3];
  ReadingForElevation(-8.5, reading);
  X_NUI_TILT_STATUS block;
  FillFromGravity(&block, reading);

  CHECK(DriverMeasuredAngle(block) == Approx(-8.5).margin(0.1));
  CHECK(block.accel_z.get() < 0);
  CHECK(block.accel_y.get() > 0);
}

TEST_CASE("A block for a motor on the move says so, and only that",
          "[nui][tilt]") {
  // The driver reads & 3 of the flags word as "the motor is still moving"
  // (0x82d2dd90, and the accessor 0x82d2c6d8) and waits instead of judging
  // an angle the head has not finished reaching. The & 4 and & 8 bits make it
  // return codes whose meaning is unknown, so they are never set.
  CHECK(kNuiTiltBlockMoving == 0x3u);
  CHECK((kNuiTiltBlockMoving & 0xCu) == 0u);

  X_NUI_TILT_STATUS still;
  X_NUI_TILT_STATUS moving;
  FillForAngle(&still, 10);
  std::memset(&moving, 0xCD, sizeof(moving));
  FillForAngle(&moving, 10, true);
  CHECK(still.status_flags.get() == 0u);
  CHECK(moving.status_flags.get() == kNuiTiltBlockMoving);
  CHECK(moving.accel_y.get() == still.accel_y.get());
  CHECK(moving.accel_z.get() == still.accel_z.get());
  CHECK(moving.cb_size.get() == kNuiTiltStatusSize);
  CHECK(moving.signature.get() == kNuiTiltStatusSignature);
  CHECK(ReservedBytesSet(moving) == 0u);

  float reading[3];
  ReadingForElevation(-25.0, reading);
  X_NUI_TILT_STATUS measured_still;
  X_NUI_TILT_STATUS measured_moving;
  FillFromGravity(&measured_still, reading);
  FillFromGravity(&measured_moving, reading, true);
  CHECK(measured_still.status_flags.get() == 0u);
  CHECK(measured_moving.status_flags.get() == kNuiTiltBlockMoving);
  CHECK(measured_moving.accel_y.get() == measured_still.accel_y.get());
  CHECK(measured_moving.accel_z.get() == measured_still.accel_z.get());
}

TEST_CASE("A settled sensor reports no tilt status bits", "[nui][tilt]") {
  CHECK(kNuiTiltStatusOk == 0u);
  CHECK(NuiTiltStatusWord(false, false) == kNuiTiltStatusOk);
}

TEST_CASE("The two tilt status bits the title tests are distinct",
          "[nui][tilt]") {
  // 4D5308ED tests bit 0x2 at 0x82a5f864 and bit 0x1 at 0x82a596bc,
  // so the two have to be separate flags rather than two names for one.
  CHECK(kNuiTiltStatusBusy == 0x1u);
  CHECK(kNuiTiltStatusNeedsAttention == 0x2u);
  CHECK((kNuiTiltStatusBusy & kNuiTiltStatusNeedsAttention) == 0u);

  CHECK(NuiTiltStatusWord(true, false) == kNuiTiltStatusBusy);
  CHECK(NuiTiltStatusWord(false, true) == kNuiTiltStatusNeedsAttention);
  CHECK(NuiTiltStatusWord(true, true) ==
        (kNuiTiltStatusBusy | kNuiTiltStatusNeedsAttention));
}

TEST_CASE("A tilt angle survives the trip through the guest dword",
          "[nui][tilt]") {
  // The export hands the guest a DWORD, and the title reads it back as a LONG
  // and compares its magnitude against a threshold, so every angle the sensor
  // can hold has to come back out unchanged.
  CHECK(NuiTiltAngleWord(-25) == 0xFFFFFFE7u);
  CHECK(static_cast<int32_t>(NuiTiltAngleWord(-25)) == -25);
  CHECK(static_cast<int32_t>(NuiTiltAngleWord(0)) == 0);

  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    REQUIRE(static_cast<int32_t>(NuiTiltAngleWord(degrees)) == degrees);
  }
}

TEST_CASE("The angle of a settled sensor is not a status word", "[nui][tilt]") {
  // The regression this pair of helpers exists to stop: writing the angle
  // where the status belongs. The title sets the sensor to -25 degrees during
  // start-up, and 0xFFFFFFE7 has both of the bits it tests set, which pinned
  // its engagement state machine in "check the sensor" for the whole run.
  CHECK((NuiTiltAngleWord(-25) & kNuiTiltStatusNeedsAttention) != 0u);
  CHECK((NuiTiltAngleWord(-25) & kNuiTiltStatusBusy) != 0u);

  CHECK((NuiTiltStatusWord(false, false) & kNuiTiltStatusNeedsAttention) == 0u);
  CHECK((NuiTiltStatusWord(false, false) & kNuiTiltStatusBusy) == 0u);

  // Only the thirteen multiples of four among the fifty-five angles a Kinect
  // can hold would read as "nothing is wrong" if they were mistaken for a
  // status word, which is why the swap could never have been harmless.
  uint32_t angles_with_a_status_bit = 0;
  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    if (NuiTiltAngleWord(degrees) &
        (kNuiTiltStatusBusy | kNuiTiltStatusNeedsAttention)) {
      ++angles_with_a_status_bit;
    }
  }
  CHECK(angles_with_a_status_bit == 42u);
}

TEST_CASE("A floor is remembered with the legs the block carries",
          "[nui][tilt]") {
  // XamNuiCameraRememberFloor keeps the accelerometer legs the block would
  // carry at that moment, and the runtime compares them with the block's own,
  // so the two have to be one computation, for an angle and for a reading.
  for (int32_t degrees = -27; degrees <= 27; ++degrees) {
    X_NUI_TILT_STATUS block;
    FillForAngle(&block, degrees);
    const NuiTiltStatusLegs legs = NuiTiltStatusLegsForAngle(degrees);
    REQUIRE(legs.x == block.accel_x.get());
    REQUIRE(legs.y == block.accel_y.get());
    REQUIRE(legs.z == block.accel_z.get());

    float reading[3];
    ReadingForElevation(degrees + 0.3, reading);
    FillFromGravity(&block, reading);
    const NuiTiltStatusLegs measured = NuiTiltStatusLegsFromGravity(reading);
    REQUIRE(measured.x == block.accel_x.get());
    REQUIRE(measured.y == block.accel_y.get());
    REQUIRE(measured.z == block.accel_z.get());
  }
}

TEST_CASE("The floor tail reaches the guest big-endian, d in millimetres",
          "[nui][tilt]") {
  // A plane exactly as XamNuiCameraRememberFloor decodes it, remembered while
  // the sensor sat at -12 degrees, whose legs are 0, 960 and -204.
  X_NUI_TILT_STATUS block;
  FillForAngle(&block, -12);
  REQUIRE(FillNuiTiltStatusFloor(RememberedFloorAt(FloorPlane(1360.444f), -12),
                                 &block));

  const uint8_t expected_tail[0x20] = {
      0x3D, 0x4C, 0xCC, 0xCD,  // +0x30 a = 0.05
      0x3F, 0x7D, 0x70, 0xA4,  // +0x34 b = 0.99
      0xBD, 0x8F, 0x5C, 0x29,  // +0x38 c = -0.07
      0x44, 0xAA, 0x0E, 0x35,  // +0x3C d = 1360.444 mm
      0x00, 0x00, 0x00, 0x00,  // +0x40 floor_flags = 0
      0x00, 0x00, 0x00, 0x00,  // +0x44 floor_accel_x = 0
      0x00, 0x00, 0x03, 0xC0,  // +0x48 floor_accel_y = 960
      0xFF, 0xFF, 0xFF, 0x34,  // +0x4C floor_accel_z = -204
  };
  uint8_t bytes[sizeof(X_NUI_TILT_STATUS)];
  std::memcpy(bytes, &block, sizeof(bytes));
  for (size_t i = 0; i < sizeof(expected_tail); ++i) {
    INFO("byte " << (0x30 + i));
    CHECK(bytes[0x30 + i] == expected_tail[i]);
  }

  // The unit trap: the HUD skeleton frame carries the same floor in metres,
  // 1.360444, and a tail holding that would aim the runtime 19 degrees up.
  CHECK(block.floor_plane[3].get() == 1360.444f);
  CHECK(block.floor_plane[3].get() != Approx(1.360444f));
}

TEST_CASE("The runtime aims from the tail's floor, not above level",
          "[nui][tilt]") {
  // With a floor handed back the runtime's target is absolute, for each of
  // its three margins (the table at 0x8381e324: 0, 150 and 300 mm), where a
  // tail of zeros made it sweep and then add +4 or +8 to a base of zero.
  X_NUI_TILT_STATUS block;
  FillForAngle(&block, -12);
  REQUIRE(FillNuiTiltStatusFloor(RememberedFloorAt(FloorPlane(1400.0f), -12),
                                 &block));
  CHECK(DriverFloorTarget(block, 0.0) == -12);
  CHECK(DriverFloorTarget(block, 150.0) == -8);
  CHECK(DriverFloorTarget(block, 300.0) == -5);

  REQUIRE(FillNuiTiltStatusFloor(RememberedFloorAt(FloorPlane(1360.444f), -12),
                                 &block));
  CHECK(DriverFloorTarget(block, 0.0) == -11);
  CHECK(DriverFloorTarget(block, 150.0) == -7);
  CHECK(DriverFloorTarget(block, 300.0) == -4);

  // The same floor in metres, the trap above, puts every target above level.
  X_NUI_TILT_STATUS metres = block;
  metres.floor_plane[3] = 1.360444f;
  CHECK(DriverFloorTarget(metres, 0.0) == 17);
  CHECK(DriverFloorTarget(metres, 150.0) == 19);
  CHECK(DriverFloorTarget(metres, 300.0) == 21);
}

TEST_CASE("Writing the floor tail leaves the rest of the block alone",
          "[nui][tilt]") {
  // Everything before +0x30 is the fill's, the moving bits included.
  X_NUI_TILT_STATUS plain;
  X_NUI_TILT_STATUS with_floor;
  FillForAngle(&plain, 5, true);
  FillForAngle(&with_floor, 5, true);
  REQUIRE(FillNuiTiltStatusFloor(RememberedFloorAt(FloorPlane(1450.0f), -12),
                                 &with_floor));
  CHECK(std::memcmp(&plain, &with_floor, 0x30) == 0);

  // The helper touches nothing before +0x30 even in a block nobody filled,
  // and writes every byte from there on, the flags word's zero included.
  X_NUI_TILT_STATUS scribbled;
  std::memset(&scribbled, 0xCD, sizeof(scribbled));
  REQUIRE(FillNuiTiltStatusFloor(RememberedFloorAt(FloorPlane(1450.0f), -12),
                                 &scribbled));
  uint8_t bytes[sizeof(X_NUI_TILT_STATUS)];
  std::memcpy(bytes, &scribbled, sizeof(bytes));
  for (size_t i = 0; i < 0x30; ++i) {
    INFO("byte " << i);
    REQUIRE(bytes[i] == 0xCD);
  }
  CHECK(scribbled.floor_plane[3].get() == 1450.0f);
  CHECK(scribbled.floor_flags.get() == 0u);
  CHECK(scribbled.floor_accel_x.get() == 0);
}

TEST_CASE("A block with no floor to hand back keeps its tail zero",
          "[nui][tilt]") {
  // Nothing remembered - the empty record, which is also what a title's end
  // leaves behind - and floors the runtime could not aim from: a d of zero
  // is its own "no floor", and a negative or non-finite one is no height at
  // all. A plane whose legs could not be read is not handed back either,
  // because the runtime would rotate it from a gravity of nothing.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  NuiRememberedFloor no_legs;
  no_legs.plane = FloorPlane(1360.444f);
  const NuiRememberedFloor floors[] = {
      NuiRememberedFloor(),
      RememberedFloorAt(FloorPlane(0.0f), -12),
      RememberedFloorAt(FloorPlane(-0.0f), -12),
      RememberedFloorAt(FloorPlane(-1360.444f), -12),
      RememberedFloorAt(FloorPlane(nan), -12),
      RememberedFloorAt(FloorPlane(inf), -12),
      RememberedFloorAt({nan, 0.99f, -0.07f, 1360.444f}, -12),
      no_legs,
  };
  for (size_t i = 0; i < std::size(floors); ++i) {
    INFO("floor " << i);
    X_NUI_TILT_STATUS block;
    std::memset(&block, 0xCD, sizeof(block));
    FillForAngle(&block, -12);
    X_NUI_TILT_STATUS filled;
    std::memcpy(&filled, &block, sizeof(filled));

    REQUIRE_FALSE(FillNuiTiltStatusFloor(floors[i], &block));
    REQUIRE(ReservedBytesSet(block) == 0u);
    REQUIRE(std::memcmp(&block, &filled, sizeof(block)) == 0);
  }
}

TEST_CASE("The tail keeps the accelerometer of the moment it was remembered",
          "[nui][tilt]") {
  // The title remembers the floor right after a poll, and the legs kept with
  // it are the ones that poll's block carried. When the head then moves, the
  // block's own legs follow it and the tail's stay: the runtime rotates the
  // remembered plane by the difference (0x82d2d540).
  X_NUI_TILT_STATUS block;
  FillForAngle(&block, -20);
  const NuiRememberedFloor floor = RememberedFloorAt(FloorPlane(1400.0f), -20);
  FillForAngle(&block, 3);
  REQUIRE(FillNuiTiltStatusFloor(floor, &block));
  CHECK(DriverMeasuredAngle(block) == Approx(3.0).margin(0.5));
  CHECK(DriverRememberedAngle(block) == Approx(-20.0).margin(0.5));
  CHECK(block.floor_accel_y.get() == NuiTiltStatusLegsForAngle(-20).y);
  CHECK(block.floor_accel_z.get() == NuiTiltStatusLegsForAngle(-20).z);
  CHECK(block.accel_z.get() == NuiTiltStatusLegsForAngle(3).z);

  // The same with a measured accelerometer, remembered at -17.8 degrees and
  // read back after the head has come up to -7.
  float remembered_reading[3];
  float current_reading[3];
  ReadingForElevation(-17.8, remembered_reading);
  ReadingForElevation(-7.0, current_reading);
  NuiRememberedFloor measured;
  measured.plane = FloorPlane(1400.0f);
  measured.legs = NuiTiltStatusLegsFromGravity(remembered_reading);
  FillFromGravity(&block, current_reading);
  REQUIRE(FillNuiTiltStatusFloor(measured, &block));
  CHECK(DriverMeasuredAngle(block) == Approx(-7.0).margin(0.1));
  CHECK(DriverRememberedAngle(block) == Approx(-17.8).margin(0.1));
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
