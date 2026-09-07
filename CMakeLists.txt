cmake_minimum_required(VERSION 3.20)
project(winios C CXX)

# zlib: the importer needs deflate to read a zip, and every platform this
# actually ships on has it -- iOS, macOS and Linux all do -- so it is a
# find_package rather than a vendored copy.
#
# Not REQUIRED, though, because it is findable for the *host* and not
# necessarily for a cross target: the aarch64-linux cross build used to check
# the dynarec has no arm64 zlib in its sysroot, and losing that check to gain
# the importer would be a bad trade. Where zlib is missing the importer is
# skipped and everything else builds as before.
find_package(ZLIB)

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

# --- xc_bench: interpreter vs dynarec timing on the host CPU -----------------
add_executable(test_bench tests/test_bench.c)
target_link_libraries(test_bench PRIVATE xcore_golden)
add_test(NAME bench COMMAND test_bench 20000)

# --- 32-bit arena mode: portable, runs everywhere ----------------------------
add_executable(test_arena32 tests/test_arena32.c)
target_link_libraries(test_arena32 PRIVATE xcore)
add_test(NAME arena32 COMMAND test_arena32)

# Replays the recorded silicon post-states. On CI this is redundant with
# difftest; on an iPhone it is the only oracle there is.
add_executable(test_selftest tests/test_selftest.c)
target_link_libraries(test_selftest PRIVATE xcore_golden)
add_test(NAME selftest COMMAND test_selftest)

# JIT vs interpreter on every difftest case (passes trivially where there is no JIT)
add_executable(test_jitdiff tests/test_jitdiff.c)
target_link_libraries(test_jitdiff PRIVATE xcore)
target_include_directories(test_jitdiff PRIVATE tests)
add_test(NAME jitdiff COMMAND test_jitdiff)

# What a fault inside a compiled block reports, against what x86 says it must
# (see the file). Also passes trivially where there is no JIT.
add_executable(test_faultdiff tests/test_faultdiff.c)
target_link_libraries(test_faultdiff PRIVATE xcore)
target_include_directories(test_faultdiff PRIVATE tests)
add_test(NAME faultdiff COMMAND test_faultdiff)

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

# winrun: Windows executables (PE32 / PE32+) on xcore with a host-implemented
# kernel32 / msvcrt (win32/). The .exe guests under tests/win32/ are prebuilt
# with mingw-w64 so nothing beyond a C compiler is needed to run them.
if(NOT WIN32)
    # The same sources twice: once as the command-line tool, once as a library
    # the device build links (tools/memprobe) so an iPad can run a .exe.
    add_library(winrun_lib STATIC tools/winrun/winrun.c win32/pe.c win32/kernel32.c win32/msvcrt.c win32/com.c win32/d3d9.c win32/raster.c win32/stdcall_args.c win32/advapi32.c win32/seh.c win32/user32.c win32/dsound.c win32/xinput.c win32/thread.c win32/shell32.c)
    target_link_libraries(winrun_lib PUBLIC xcore)
    if(NOT APPLE)
        target_link_libraries(winrun_lib PUBLIC m)
    endif()
    target_include_directories(winrun_lib PUBLIC win32 tools/winrun)
    target_compile_definitions(winrun_lib PRIVATE WINRUN_NO_MAIN=1)
    set_property(TARGET winrun_lib PROPERTY C_STANDARD 11)
    target_compile_options(winrun_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)

    add_executable(winrun tools/winrun/winrun.c win32/pe.c win32/kernel32.c win32/msvcrt.c win32/com.c win32/d3d9.c win32/raster.c win32/stdcall_args.c win32/advapi32.c win32/seh.c win32/user32.c win32/dsound.c win32/xinput.c win32/thread.c win32/shell32.c)
    target_link_libraries(winrun PRIVATE xcore m)
    target_include_directories(winrun PRIVATE win32)
    set_property(TARGET winrun PROPERTY C_STANDARD 11)
    target_compile_options(winrun PRIVATE -Wall -Wextra -Wno-unused-parameter)

    # --- the importer: bringing a program in from outside ---------------------
    if(ZLIB_FOUND)
        # Its own library so the app links the same code the CLI tests, and so the
        # parts that have nothing to do with the emulator (a zip reader, installer
        # detection, a drive diff) can be tested without starting a guest.
        add_library(import_lib STATIC
            tools/import/setupkind.c tools/import/unzip.c
            tools/import/drivediff.c tools/import/import.c)
        target_include_directories(import_lib PUBLIC tools/import)
        target_link_libraries(import_lib PUBLIC ZLIB::ZLIB)
        set_property(TARGET import_lib PROPERTY C_STANDARD 11)
        target_compile_options(import_lib PRIVATE -Wall -Wextra)

        add_executable(wimport tools/wimport/wimport.c)
        target_link_libraries(wimport PRIVATE import_lib winrun_lib)
        set_property(TARGET wimport PROPERTY C_STANDARD 11)
        target_compile_options(wimport PRIVATE -Wall -Wextra)

        # Links winrun_lib as well, though it never starts a guest: installer mode
        # has to create the drive skeleton before a setup program looks for
        # Program Files, and that lives in the win32 layer. A header dependency
        # would have been worth avoiding; a link dependency is not.
        add_executable(test_import tests/test_import.c)
        target_link_libraries(test_import PRIVATE import_lib winrun_lib)
        target_include_directories(test_import PRIVATE tests)
        add_test(NAME import COMMAND test_import ${CMAKE_CURRENT_SOURCE_DIR}/tests/import)

        # Installer mode end to end, against a guest that behaves like a silent
        # install and refuses to work if it was handed the wrong flags.
        add_test(NAME import_installer COMMAND ${CMAKE_COMMAND} -E env
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/import/install.sh
            $<TARGET_FILE:wimport> ${CMAKE_CURRENT_SOURCE_DIR}/tests/win32)
    else()
        message(STATUS "zlib not found for this target: skipping the importer "
                       "(tools/import, wimport). Everything else is unaffected.")
    endif()

    # Several guests back to back in one process -- what the device build does
    # when the button is tapped again, and what would break first if
    # winrun_reset() ever missed a global.
    add_executable(test_winrun_lib tests/test_winrun_lib.c)
    target_link_libraries(test_winrun_lib PRIVATE winrun_lib)
    add_test(NAME winrun_lib COMMAND test_winrun_lib ${CMAKE_CURRENT_SOURCE_DIR}/tests/win32)
    add_test(NAME win32_guests COMMAND ${CMAKE_COMMAND} -E env
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/win32/run.sh $<TARGET_FILE:winrun> ${CMAKE_CURRENT_SOURCE_DIR}/tests/win32)
endif()
