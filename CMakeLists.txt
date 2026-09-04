cmake_minimum_required(VERSION 3.20)
project(winios C CXX)

enable_testing()

add_subdirectory(core)

# --- GPU: Direct3D 12 -> Metal compiler (own repo, vendored as a submodule) ---
# `git submodule update --init --recursive` fetches it, plus dxil-spirv and
# SPIRV-Cross underneath it. Without the submodule the build simply skips it.
option(WINIOS_D12MT "Build the D3D12 -> Metal compiler" ON)
if(WINIOS_D12MT AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gpu/d12mt/CMakeLists.txt)
    add_subdirectory(gpu/d12mt)
endif()

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

# --- AArch64 encoder sanity: emits one of everything; bytes are checked by
# hand against the system disassembler (see the file) -----------------------
add_executable(test_a64 tests/test_a64.c)
add_test(NAME a64_encoder COMMAND test_a64)

# --- differential test: needs a real x86-64 to act as the oracle -------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    enable_language(ASM)
    add_executable(difftest tests/difftest/difftest.c tests/difftest/native_x64.S)
    target_link_libraries(difftest PRIVATE xcore)
    add_test(NAME difftest COMMAND difftest)
endif()

# --- xrun: static x86-64 Linux ELF on xcore (Linux host only for now) --------
# The guest programs under tests/guest/ are prebuilt (musl, static-pie) so no
# cross toolchain is needed to run them; tests/guest/build.sh rebuilds them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(xrun tools/xrun/xrun.c tools/xrun/linux32.c)
    target_link_libraries(xrun PRIVATE xcore)
    set_property(TARGET xrun PROPERTY C_STANDARD 11)
    target_compile_options(xrun PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME guest_hello COMMAND ${CMAKE_COMMAND} -E env
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run.sh $<TARGET_FILE:xrun> ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest)
endif()
