xe_test_suite(xenia-kernel-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS fmt mspack xenia-base
)

# The LZX decompressor behind the kernel's LDI exports, the Ob layer's rules
# for title-defined object types, and xam's Kinect Guide gesture recogniser,
# frame reader and per-frame decisions are host-only code, so the suite
# compiles them in rather than linking the whole kernel. The recogniser's cvar
# helper, nui_guide_gesture_cvars.cc, stays out: the suite does not link the
# cvar definitions.
if(TARGET xenia-kernel-tests)
  target_sources(xenia-kernel-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../xam/nui_guide_frame.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../xam/nui_guide_gesture.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../xam/nui_guide_session.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../xboxkrnl/guest_object_type.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../xboxkrnl/lzx_decompressor.cc
  )
endif()
