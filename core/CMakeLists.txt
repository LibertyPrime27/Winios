# xcore -- the CPU core. Portable C11, no Apple or Linux headers, so it builds
# identically for iOS arm64, macOS, and the Linux CI runner.

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zydis ${CMAKE_CURRENT_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)

add_library(xcore STATIC
    src/mem.c
    src/interp.c)
target_include_directories(xcore PUBLIC include)
target_link_libraries(xcore PUBLIC Zydis)
set_property(TARGET xcore PROPERTY C_STANDARD 11)
target_compile_options(xcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
