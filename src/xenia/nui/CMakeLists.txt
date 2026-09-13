add_library(xenia-nui STATIC)
xe_platform_sources(xenia-nui ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-nui PUBLIC xenia-base)
xe_target_defaults(xenia-nui)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
