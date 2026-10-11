/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_
#define XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_

#include <cstdint>
#include <span>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The Ob layer's rules for object types a title defines itself.
//
// A title can define an object type of its own, an X_OBJECT_TYPE in its image
// with allocate, free, close and delete procedures, and create objects of it
// with ObCreateObject, which puts an X_OBJECT_HEADER naming that type in front
// of each object. The console's kernel checks references against that header
// and runs the type's procedures. xenia keeps a host-side XObject for every
// object a title holds a handle to, with a type of xenia's own, so these rules
// are where the two meet. They take plain values, so that they can be tested
// without a kernel; the export shims read the values from guest memory.

// Whether the type pointer read from an object's guest header names a type the
// title defined.
//
// type_ptr is 0 when the object has no guest address or its header cannot be
// read, type_readable says whether the X_OBJECT_TYPE it points at can be read,
// and allocate_proc is that type's allocate procedure. [xenia_types_begin,
// xenia_types_end) is where the kernel's guest globals hold the object types
// xenia itself defines; their objects keep today's behaviour. Every type
// ObCreateObject creates objects of has an allocate procedure, because that is
// what makes the object; the private type xenia writes into a header it
// allocates itself (XObject::CreateNative) is zero-filled, so it has none.
bool IsTitleObjectType(uint32_t type_ptr, bool type_readable,
                       uint32_t allocate_proc, uint32_t xenia_types_begin,
                       uint32_t xenia_types_end);

// How ObReferenceObjectByHandle answers a caller that names the object type it
// expects.
enum class ObjectTypeCheck {
  // xenia's own type for the object is the caller's.
  kHostTypeMatches,
  // The object's own guest header names the caller's type: the title created
  // the object with ObCreateObject and a type of its own.
  kGuestTypeMatches,
  // xenia knows the object's type and it is not the caller's.
  kMismatch,
  // Neither xenia nor the object's header knows the caller's type.
  kUnknown,
};

// requested_type is the caller's X_OBJECT_TYPE pointer and is never 0 here.
// host_type is the guest X_OBJECT_TYPE xenia has for the object's host type,
// or 0 when it has none. header_type is the type recorded by
// ObOpenObjectByPointer, or 0 when none was recorded. A recorded guest type
// takes precedence over the host wrapper's type.
ObjectTypeCheck CheckObjectType(uint32_t requested_type, uint32_t host_type,
                                uint32_t header_type);

// What closing a handle to an object of a title-defined type does with the
// type's close procedure.
enum class CloseProcedureAction {
  // Nothing to run: the type has no close procedure, or the handle no longer
  // names the object.
  kNone,
  // Run it.
  kRun,
  // The procedure is not guest code, so the header it came from is not one
  // ObCreateObject wrote; it is not run.
  kRefuse,
};

// The console's kernel runs the close procedure on every close and hands it
// the handle count as it stood before the close, so 1 means "this was the
// last handle"; the procedure itself decides what a close that is not the
// last one does. handle_count is that count, and 0 or less means the handle
// no longer names the object. close_proc_is_code says whether close_proc is
// the entry of guest code, readable and inside a loaded module.
CloseProcedureAction PlanCloseProcedure(uint32_t close_proc,
                                        bool close_proc_is_code,
                                        int32_t handle_count);

// Whether ObOpenObjectByPointer takes a handle reference on top of the handle
// it hands out. A wrapper that XObject::GetNativeObject creates holds the one
// handle its constructor added, whether ObOpenObjectByPointer made it or a
// kernel call on the object did, and for an object of a title-defined type
// the first open hands that handle to the title as it is, so its count
// reaches zero on the title's NtClose; every later open takes a reference of
// its own. handle_handed_out says whether an open has handed the handle out
// already. Every other object keeps the extra reference it has always been
// given.
bool OpenByPointerRetainsHandle(bool handle_handed_out, bool title_object_type);

// What becomes of the handle a wrapped guest object carries in its dispatch
// header (XObject::GetNativeObject) once one of its handles has been released
// for the last time. A freed handle slot goes to the next object added, so a
// header left naming a closed handle would lead the next kernel call on the
// object to whatever object holds that slot by then.
enum class StashedHandleAction {
  // Leave the header alone.
  kKeep,
  // Point it at another handle the object still has.
  kRepoint,
  // Give it back an empty wait list, so that the next kernel call on the
  // object wraps it afresh.
  kReset,
};

struct StashedHandleDecision {
  StashedHandleAction action;
  // The handle to point the header at, for kRepoint.
  uint32_t handle;
};

// closed_handle is the handle whose last reference was released, and
// remaining_handles the handles the object has left. header_has_stash says
// whether the header still carries xenia's stash at all (a title can
// re-initialise the object in place), and stashed_handle is the handle in it.
// stashed_handle_wraps_same_object says whether that handle now names a live
// wrapper of the same guest object: its slot can have gone to a new wrapper
// of the object before this runs, and that wrapper's stash is its own.
StashedHandleDecision DecideStashedHandle(
    uint32_t closed_handle, std::span<const uint32_t> remaining_handles,
    bool header_has_stash, uint32_t stashed_handle,
    bool stashed_handle_wraps_same_object);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_
