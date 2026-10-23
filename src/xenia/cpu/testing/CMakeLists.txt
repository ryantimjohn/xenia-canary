xe_test_suite(xenia-cpu-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui
    xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-kernel xenia-ui xenia-patcher
    $<$<BOOL:${XE_TARGET_X86_64}>:xenia-cpu-backend-x64>
    $<$<BOOL:${XE_TARGET_AARCH64}>:xenia-cpu-backend-a64>
)

# Backend/compiler headers used by tests include the bundled LLVM ADT headers.
target_include_directories(xenia-cpu-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/llvm/include
)

# Only the CPU executable admits the private scalar death-test child switch.
if(WIN32 AND XE_TARGET_X86_64)
  target_compile_definitions(xenia-cpu-tests PRIVATE XE_CPU_FAULT_CHILD=1)
endif()
