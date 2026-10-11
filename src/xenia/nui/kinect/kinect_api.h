/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_KINECT_KINECT_API_H_
#define XENIA_NUI_KINECT_KINECT_API_H_

#include <cstdint>

#include "xenia/base/platform_win.h"

namespace xe {
namespace nui {
namespace kinect {
namespace api {

// Our own declarations of the flat C entry points that the Kinect for Windows
// runtime exports from Kinect10.dll. Microsoft's public Kinect for Windows
// 1.8 NUI function and INuiSensor references are linked in docs/nui.md under
// "Requirements and backend selection". No Kinect SDK headers or import
// libraries are needed to build or link; the DLL is loaded at runtime and
// every entry point below is resolved by name.

// Subsystems that NuiInitialize should bring up. We only ever want depth.
inline constexpr DWORD kInitializeUsesDepth = 0x00000020;

// Image types accepted by NuiImageStreamOpen.
inline constexpr int kImageTypeDepthAndPlayerIndex = 0;
inline constexpr int kImageTypeColor = 1;
inline constexpr int kImageTypeColorYuv = 2;
inline constexpr int kImageTypeColorRawYuv = 3;
inline constexpr int kImageTypeDepth = 4;

// Image resolutions accepted by NuiImageStreamOpen.
inline constexpr int kResolutionInvalid = -1;
inline constexpr int kResolution80x60 = 0;
inline constexpr int kResolution320x240 = 1;
inline constexpr int kResolution640x480 = 2;
inline constexpr int kResolution1280x960 = 3;

// Number of frames the runtime may buffer for us before it drops one.
inline constexpr DWORD kDepthFrameLimit = 2;

// A depth pixel is (distance in millimetres << 3) | player index, so the
// distance needs shifting out of the low bits.
inline constexpr int kDepthPixelDistanceShift = 3;
// A shifted value this large is the sensor saying "too far away" or "no idea",
// not a distance it actually measured.
inline constexpr uint16_t kDepthDistanceUnknown = 0x0FFF;

// NuiImageStreamGetNextFrame's way of saying that no frame is waiting yet.
// The stream can signal before a frame has finished, so this is the normal
// answer to a read that arrives too early, not an error.
inline constexpr HRESULT kErrorFrameNoData = static_cast<HRESULT>(0x83010001L);

// Where NuiImageStreamGetNextFrame's pixels live once a texture is locked.
struct LockedRect {
  INT pitch;
  INT size;
  void* bits;
};

// The part of the sensor's field of view that a frame covers.
struct ImageViewArea {
  int digital_zoom;
  LONG center_x;
  LONG center_y;
};

// The texture that carries an image frame's pixels. The order of the methods
// below is the ABI the runtime was compiled against, so it must not change.
struct IFrameTexture : public IUnknown {
  virtual int STDMETHODCALLTYPE BufferLen() = 0;
  virtual int STDMETHODCALLTYPE Pitch() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRect(UINT level, LockedRect* rect,
                                             RECT* subrect, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, void* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) = 0;
};

// One frame handed out by NuiImageStreamGetNextFrame. It stays owned by the
// runtime until NuiImageStreamReleaseFrame is called with it.
struct ImageFrame {
  LARGE_INTEGER timestamp;
  DWORD frame_number;
  int image_type;
  int resolution;
  IFrameTexture* texture;
  DWORD frame_flags;
  ImageViewArea view_area;
};

// Four floats as the runtime hands them over. The accelerometer fills x, y
// and z in g and always leaves w at zero.
struct Vector4 {
  float x;
  float y;
  float z;
  float w;
};
static_assert(sizeof(Vector4) == 16, "Vector4 is four packed floats");

// The first sensor, the one the flat entry points above drive.
inline constexpr int kDefaultSensorIndex = 0;

// The per-sensor interface NuiCreateSensorByIndex hands out. It is the only
// way the runtime offers to read the sensor's accelerometer; there is no flat
// entry point for that. What matters here is where each method sits in the
// object's table of functions, which is fixed by the order below: it is the
// order the 1.8 runtime lays the interface out in, after IUnknown's three,
// and must not change. The three methods this backend calls are declared
// with their real signatures. Every other slot up to the last of them is
// declared by its method's name alone, taking and returning nothing, so that
// the three land where the runtime put them; none of those may ever be
// called through this declaration. The slot numbers below count IUnknown's
// three as 0 to 2.
// The public method reference documents signatures, not vtable slot numbers.
// See docs/nui.md, "Where the angle comes from: the sensor's accelerometer",
// for the 1.8 runtime observations and checks used before calling these slots.
struct ISensor : public IUnknown {
  virtual void STDMETHODCALLTYPE NuiInitialize() = 0;
  virtual void STDMETHODCALLTYPE NuiShutdown() = 0;
  virtual void STDMETHODCALLTYPE NuiSetFrameEndEvent() = 0;
  virtual void STDMETHODCALLTYPE NuiImageStreamOpen() = 0;
  virtual void STDMETHODCALLTYPE NuiImageStreamSetImageFrameFlags() = 0;
  virtual void STDMETHODCALLTYPE NuiImageStreamGetImageFrameFlags() = 0;
  virtual void STDMETHODCALLTYPE NuiImageStreamGetNextFrame() = 0;
  virtual void STDMETHODCALLTYPE NuiImageStreamReleaseFrame() = 0;
  virtual void STDMETHODCALLTYPE
  NuiImageGetColorPixelCoordinatesFromDepthPixel() = 0;
  virtual void STDMETHODCALLTYPE
  NuiImageGetColorPixelCoordinatesFromDepthPixelAtResolution() = 0;
  virtual void STDMETHODCALLTYPE
  NuiImageGetColorPixelCoordinateFrameFromDepthPixelFrameAtResolution() = 0;
  virtual void STDMETHODCALLTYPE NuiCameraElevationSetAngle() = 0;
  virtual void STDMETHODCALLTYPE NuiCameraElevationGetAngle() = 0;
  virtual void STDMETHODCALLTYPE NuiSkeletonTrackingEnable() = 0;
  virtual void STDMETHODCALLTYPE NuiSkeletonTrackingDisable() = 0;
  virtual void STDMETHODCALLTYPE NuiSkeletonSetTrackedSkeletons() = 0;
  virtual void STDMETHODCALLTYPE NuiSkeletonGetNextFrame() = 0;
  virtual void STDMETHODCALLTYPE NuiTransformSmooth() = 0;
  virtual void STDMETHODCALLTYPE NuiGetAudioSource() = 0;
  // Slot 22: the zero-based index the sensor was created with.
  virtual int STDMETHODCALLTYPE NuiInstanceIndex() = 0;
  virtual void STDMETHODCALLTYPE NuiDeviceConnectionId() = 0;
  virtual void STDMETHODCALLTYPE NuiUniqueId() = 0;
  virtual void STDMETHODCALLTYPE NuiAudioArrayId() = 0;
  virtual void STDMETHODCALLTYPE NuiStatus() = 0;
  // Slot 27: the flags the sensor was initialized with.
  virtual DWORD STDMETHODCALLTYPE NuiInitializationFlags() = 0;
  virtual void STDMETHODCALLTYPE NuiGetCoordinateMapper() = 0;
  virtual void STDMETHODCALLTYPE
  NuiImageFrameGetDepthImagePixelFrameTexture() = 0;
  virtual void STDMETHODCALLTYPE NuiGetColorCameraSettings() = 0;
  virtual void STDMETHODCALLTYPE NuiGetForceInfraredEmitterOff() = 0;
  virtual void STDMETHODCALLTYPE NuiSetForceInfraredEmitterOff() = 0;
  // Slot 33: the gravity vector the accelerometer in the sensor's head
  // measures now, in g, pointing at the floor, on the sensor's axes (x to its
  // left, y up, z the way it looks), so a level sensor reads (0, -1, 0, 0).
  virtual HRESULT STDMETHODCALLTYPE
  NuiAccelerometerGetCurrentReading(Vector4* reading) = 0;
};

using PFNNuiInitialize = HRESULT(WINAPI*)(DWORD flags);
using PFNNuiShutdown = void(WINAPI*)();
using PFNNuiGetSensorCount = HRESULT(WINAPI*)(int* count);
using PFNNuiImageStreamOpen = HRESULT(WINAPI*)(int image_type, int resolution,
                                               DWORD image_frame_flags,
                                               DWORD frame_limit,
                                               HANDLE next_frame_event,
                                               HANDLE* stream_handle);
using PFNNuiImageStreamGetNextFrame = HRESULT(WINAPI*)(
    HANDLE stream, DWORD milliseconds_to_wait, const ImageFrame** frame);
using PFNNuiImageStreamReleaseFrame = HRESULT(WINAPI*)(HANDLE stream,
                                                       const ImageFrame* frame);
using PFNNuiCameraElevationGetAngle = HRESULT(WINAPI*)(LONG* degrees);
using PFNNuiCameraElevationSetAngle = HRESULT(WINAPI*)(LONG degrees);
using PFNNuiCreateSensorByIndex = HRESULT(WINAPI*)(int index, ISensor** sensor);

// Every entry point the backend needs, as resolved from a loaded Kinect10.dll.
// The last one is optional: it is null on a runtime that is too old to have
// the accelerometer, and the backend then reports the tilt angle it asked the
// motor for instead of a measured one.
struct Exports {
  PFNNuiInitialize NuiInitialize = nullptr;
  PFNNuiShutdown NuiShutdown = nullptr;
  PFNNuiGetSensorCount NuiGetSensorCount = nullptr;
  PFNNuiImageStreamOpen NuiImageStreamOpen = nullptr;
  PFNNuiImageStreamGetNextFrame NuiImageStreamGetNextFrame = nullptr;
  PFNNuiImageStreamReleaseFrame NuiImageStreamReleaseFrame = nullptr;
  PFNNuiCameraElevationGetAngle NuiCameraElevationGetAngle = nullptr;
  PFNNuiCameraElevationSetAngle NuiCameraElevationSetAngle = nullptr;
  PFNNuiCreateSensorByIndex NuiCreateSensorByIndex = nullptr;
};

}  // namespace api
}  // namespace kinect
}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_KINECT_KINECT_API_H_
