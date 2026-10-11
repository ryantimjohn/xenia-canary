add_library(xenia-nui-replay STATIC)
xe_platform_sources(xenia-nui-replay ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui-replay PUBLIC xenia-base xenia-nui)
xe_target_defaults(xenia-nui-replay)
