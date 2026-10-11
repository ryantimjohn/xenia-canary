add_library(xenia-nui-kinect STATIC)
xe_platform_sources(xenia-nui-kinect ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui-kinect PUBLIC xenia-base xenia-nui)
xe_target_defaults(xenia-nui-kinect)
