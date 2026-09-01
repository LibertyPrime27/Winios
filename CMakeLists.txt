cmake_minimum_required(VERSION 3.20)
project(winios C)

enable_testing()

add_subdirectory(core)

# --- MemProbe portable core (see docs/MEMPROBE.md) ---------------------------
add_library(memprobe_core STATIC tools/memprobe/Shared/memprobe.c)
target_include_directories(memprobe_core PUBLIC tools/memprobe/Shared)
set_property(TARGET memprobe_core PROPERTY C_STANDARD 11)

add_executable(test_memprobe tests/test_memprobe.c)
target_link_libraries(test_memprobe PRIVATE memprobe_core)
add_test(NAME memprobe COMMAND test_memprobe)

# --- 32-bit arena mode: portable, runs everywhere ----------------------------
add_executable(test_arena32 tests/test_arena32.c)
target_link_libraries(test_arena32 PRIVATE xcore)
add_test(NAME arena32 COMMAND test_arena32)

# Replays the recorded silicon post-states. On CI this is redundant with
# difftest; on an iPhone it is the only oracle there is.
add_executable(test_selftest tests/test_selftest.c)
target_link_libraries(test_selftest PRIVATE xcore_golden)
add_test(NAME selftest COMMAND test_selftest)

# --- differential test: needs a real x86-64 to act as the oracle -------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    enable_language(ASM)
    add_executable(difftest tests/difftest/difftest.c tests/difftest/native_x64.S)
    target_link_libraries(difftest PRIVATE xcore)
    add_test(NAME difftest COMMAND difftest)
endif()
