xe_test_suite(xenia-gpu-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    fmt imgui
    xenia-base xenia-core xenia-cpu xenia-gpu xenia-kernel xenia-patcher
    $<$<BOOL:${XE_TARGET_X86_64}>:xenia-cpu-backend-x64>
    $<$<BOOL:${XE_TARGET_AARCH64}>:xenia-cpu-backend-a64>
)
