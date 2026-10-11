xe_test_suite(xenia-kernel-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS fmt xenia-base
)

if(TARGET xenia-kernel-tests)
  target_sources(xenia-kernel-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../xboxkrnl/guest_object_type.cc
  )
endif()
