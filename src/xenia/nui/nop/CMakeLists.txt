add_library(xenia-nui-nop STATIC)
xe_platform_sources(xenia-nui-nop ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui-nop PUBLIC xenia-base xenia-nui)
xe_target_defaults(xenia-nui-nop)
