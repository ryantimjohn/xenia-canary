/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NUI_NUI_TILT_REQUEST_SLOT_H_
#define XENIA_NUI_NUI_TILT_REQUEST_SLOT_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace nui {

// Hands a title's tilt requests from whichever guest thread makes them to the
// one thread that applies them to the motor, and says whether the motor is
// on its way somewhere. Only the latest request matters: a new one replaces
// one that has not been taken yet.
//
// "Moving" covers the whole of a request's life, from Post until the
// applying thread's Finish, with no step in between at which it reads as
// stopped: Take marks the request as being applied before it stops marking
// it as waiting. Looking for work when there is none never marks anything.
//
// Post and IsMoving may be called from any thread, Take and Finish only from
// the one thread that applies requests. Clear may also be called by another
// thread that has taken that one's place, provided it has first made sure
// nothing reads IsMoving as meaningful any more (the Kinect backend marks the
// motor unavailable before it clears). The applying thread may still store
// into the slot afterwards - a Take it was in the middle of, then a Finish -
// and nothing it stores is read.
class TiltRequestSlot {
 public:
  // Asks for the motor to go to the given angle.
  void Post(int32_t degrees) {
    degrees_.store(degrees, std::memory_order_release);
    pending_.store(true, std::memory_order_release);
  }

  // Takes the latest request, if there is one. From here until Finish the
  // motor counts as moving.
  bool Take(int32_t* degrees) {
    if (!pending_.load(std::memory_order_acquire)) {
      return false;
    }
    busy_.store(true, std::memory_order_release);
    // An exchange, not a store: it reads the latest "waiting", so a Post that
    // lands while this one is being taken is either taken now, with its
    // angle, or left waiting for the next Take - never cleared unseen. At
    // worst a request is applied twice, which the gate drops.
    if (!pending_.exchange(false, std::memory_order_acq_rel)) {
      busy_.store(false, std::memory_order_release);
      return false;
    }
    *degrees = degrees_.load(std::memory_order_acquire);
    return true;
  }

  // The request that was taken has been dealt with.
  void Finish() { busy_.store(false, std::memory_order_release); }

  // True while a request is waiting or being applied.
  bool IsMoving() const {
    return pending_.load(std::memory_order_acquire) ||
           busy_.load(std::memory_order_acquire);
  }

  // Drops whatever is waiting or being applied: nothing will be applied any
  // more, and nothing is reported as moving.
  void Clear() {
    pending_.store(false, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
  }

 private:
  std::atomic<int32_t> degrees_ = {0};
  std::atomic<bool> pending_ = {false};
  std::atomic<bool> busy_ = {false};
};

}  // namespace nui
}  // namespace xe

#endif  // XENIA_NUI_NUI_TILT_REQUEST_SLOT_H_
