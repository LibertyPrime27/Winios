cmake_minimum_required(VERSION 3.20)
project(winios C)

enable_testing()

# The portable half of the probe. Kept free of Apple headers so it builds and
# is tested on a Linux runner in seconds -- a bug here would be indistinguishable
# from a low memory limit when run on device.
add_library(memprobe_core STATIC tools/memprobe/Shared/memprobe.c)
target_include_directories(memprobe_core PUBLIC tools/memprobe/Shared)
set_property(TARGET memprobe_core PROPERTY C_STANDARD 11)

add_executable(test_memprobe tests/test_memprobe.c)
target_link_libraries(test_memprobe PRIVATE memprobe_core)
add_test(NAME memprobe COMMAND test_memprobe)
