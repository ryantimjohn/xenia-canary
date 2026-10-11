xe_test_suite(xenia-nui-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS fmt xenia-base xenia-nui xenia-nui-replay
)

# The Kinect backend is built on Windows only, and so are its tests, which
# live beside it. The suite target exists only when it found test sources.
if(WIN32 AND TARGET xenia-nui-tests)
  target_sources(xenia-nui-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../kinect/testing/kinect_nui_system_test.cc
  )
  target_link_libraries(xenia-nui-tests PRIVATE xenia-nui-kinect)
endif()
