# xcore -- the CPU core. Portable C11, no Apple or Linux headers, so it builds
# identically for iOS arm64, macOS, and the Linux CI runner.

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zydis ${CMAKE_CURRENT_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)

# Berkeley SoftFloat 3e (BSD-3), 8086-SSE specialisation: bit-exact x87
# extended precision on any host. Only the extF80/f32/f64/int paths are used,
# but the whole library is small and builds in seconds.
# Source list from build/Linux-x86_64-GCC/Makefile (the *M.c no-int64 variants are not built).
set(SF ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/softfloat/source)
set(SOFTFLOAT_SRC
    ${SF}/s_eq128.c
    ${SF}/s_le128.c
    ${SF}/s_lt128.c
    ${SF}/s_shortShiftLeft128.c
    ${SF}/s_shortShiftRight128.c
    ${SF}/s_shortShiftRightJam64.c
    ${SF}/s_shortShiftRightJam64Extra.c
    ${SF}/s_shortShiftRightJam128.c
    ${SF}/s_shortShiftRightJam128Extra.c
    ${SF}/s_shiftRightJam32.c
    ${SF}/s_shiftRightJam64.c
    ${SF}/s_shiftRightJam64Extra.c
    ${SF}/s_shiftRightJam128.c
    ${SF}/s_shiftRightJam128Extra.c
    ${SF}/s_shiftRightJam256M.c
    ${SF}/s_countLeadingZeros8.c
    ${SF}/s_countLeadingZeros16.c
    ${SF}/s_countLeadingZeros32.c
    ${SF}/s_countLeadingZeros64.c
    ${SF}/s_add128.c
    ${SF}/s_add256M.c
    ${SF}/s_sub128.c
    ${SF}/s_sub256M.c
    ${SF}/s_mul64ByShifted32To128.c
    ${SF}/s_mul64To128.c
    ${SF}/s_mul128By32.c
    ${SF}/s_mul128To256M.c
    ${SF}/s_approxRecip_1Ks.c
    ${SF}/s_approxRecip32_1.c
    ${SF}/s_approxRecipSqrt_1Ks.c
    ${SF}/s_approxRecipSqrt32_1.c
    ${SF}/8086-SSE/softfloat_raiseFlags.c
    ${SF}/8086-SSE/s_f16UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToF16UI.c
    ${SF}/8086-SSE/s_propagateNaNF16UI.c
    ${SF}/8086-SSE/s_bf16UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToBF16UI.c
    ${SF}/8086-SSE/s_f32UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToF32UI.c
    ${SF}/8086-SSE/s_propagateNaNF32UI.c
    ${SF}/8086-SSE/s_f64UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToF64UI.c
    ${SF}/8086-SSE/s_propagateNaNF64UI.c
    ${SF}/8086-SSE/extF80M_isSignalingNaN.c
    ${SF}/8086-SSE/s_extF80UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToExtF80UI.c
    ${SF}/8086-SSE/s_propagateNaNExtF80UI.c
    ${SF}/8086-SSE/f128M_isSignalingNaN.c
    ${SF}/8086-SSE/s_f128UIToCommonNaN.c
    ${SF}/8086-SSE/s_commonNaNToF128UI.c
    ${SF}/8086-SSE/s_propagateNaNF128UI.c
    ${SF}/s_roundToUI32.c
    ${SF}/s_roundToUI64.c
    ${SF}/s_roundToI32.c
    ${SF}/s_roundToI64.c
    ${SF}/s_normSubnormalBF16Sig.c
    ${SF}/s_roundPackToBF16.c
    ${SF}/s_normSubnormalF16Sig.c
    ${SF}/s_roundPackToF16.c
    ${SF}/s_normRoundPackToF16.c
    ${SF}/s_addMagsF16.c
    ${SF}/s_subMagsF16.c
    ${SF}/s_mulAddF16.c
    ${SF}/s_normSubnormalF32Sig.c
    ${SF}/s_roundPackToF32.c
    ${SF}/s_normRoundPackToF32.c
    ${SF}/s_addMagsF32.c
    ${SF}/s_subMagsF32.c
    ${SF}/s_mulAddF32.c
    ${SF}/s_normSubnormalF64Sig.c
    ${SF}/s_roundPackToF64.c
    ${SF}/s_normRoundPackToF64.c
    ${SF}/s_addMagsF64.c
    ${SF}/s_subMagsF64.c
    ${SF}/s_mulAddF64.c
    ${SF}/s_normSubnormalExtF80Sig.c
    ${SF}/s_roundPackToExtF80.c
    ${SF}/s_normRoundPackToExtF80.c
    ${SF}/s_addMagsExtF80.c
    ${SF}/s_subMagsExtF80.c
    ${SF}/s_normSubnormalF128Sig.c
    ${SF}/s_roundPackToF128.c
    ${SF}/s_normRoundPackToF128.c
    ${SF}/s_addMagsF128.c
    ${SF}/s_subMagsF128.c
    ${SF}/s_mulAddF128.c
    ${SF}/softfloat_state.c
    ${SF}/ui32_to_f16.c
    ${SF}/ui32_to_f32.c
    ${SF}/ui32_to_f64.c
    ${SF}/ui32_to_extF80.c
    ${SF}/ui32_to_extF80M.c
    ${SF}/ui32_to_f128.c
    ${SF}/ui32_to_f128M.c
    ${SF}/ui64_to_f16.c
    ${SF}/ui64_to_f32.c
    ${SF}/ui64_to_f64.c
    ${SF}/ui64_to_extF80.c
    ${SF}/ui64_to_extF80M.c
    ${SF}/ui64_to_f128.c
    ${SF}/ui64_to_f128M.c
    ${SF}/i32_to_f16.c
    ${SF}/i32_to_f32.c
    ${SF}/i32_to_f64.c
    ${SF}/i32_to_extF80.c
    ${SF}/i32_to_extF80M.c
    ${SF}/i32_to_f128.c
    ${SF}/i32_to_f128M.c
    ${SF}/i64_to_f16.c
    ${SF}/i64_to_f32.c
    ${SF}/i64_to_f64.c
    ${SF}/i64_to_extF80.c
    ${SF}/i64_to_extF80M.c
    ${SF}/i64_to_f128.c
    ${SF}/i64_to_f128M.c
    ${SF}/bf16_isSignalingNaN.c
    ${SF}/bf16_to_f32.c
    ${SF}/f16_to_ui32.c
    ${SF}/f16_to_ui64.c
    ${SF}/f16_to_i32.c
    ${SF}/f16_to_i64.c
    ${SF}/f16_to_ui32_r_minMag.c
    ${SF}/f16_to_ui64_r_minMag.c
    ${SF}/f16_to_i32_r_minMag.c
    ${SF}/f16_to_i64_r_minMag.c
    ${SF}/f16_to_f32.c
    ${SF}/f16_to_f64.c
    ${SF}/f16_to_extF80.c
    ${SF}/f16_to_extF80M.c
    ${SF}/f16_to_f128.c
    ${SF}/f16_to_f128M.c
    ${SF}/f16_roundToInt.c
    ${SF}/f16_add.c
    ${SF}/f16_sub.c
    ${SF}/f16_mul.c
    ${SF}/f16_mulAdd.c
    ${SF}/f16_div.c
    ${SF}/f16_rem.c
    ${SF}/f16_sqrt.c
    ${SF}/f16_eq.c
    ${SF}/f16_le.c
    ${SF}/f16_lt.c
    ${SF}/f16_eq_signaling.c
    ${SF}/f16_le_quiet.c
    ${SF}/f16_lt_quiet.c
    ${SF}/f16_isSignalingNaN.c
    ${SF}/f32_to_ui32.c
    ${SF}/f32_to_ui64.c
    ${SF}/f32_to_i32.c
    ${SF}/f32_to_i64.c
    ${SF}/f32_to_ui32_r_minMag.c
    ${SF}/f32_to_ui64_r_minMag.c
    ${SF}/f32_to_i32_r_minMag.c
    ${SF}/f32_to_i64_r_minMag.c
    ${SF}/f32_to_bf16.c
    ${SF}/f32_to_f16.c
    ${SF}/f32_to_f64.c
    ${SF}/f32_to_extF80.c
    ${SF}/f32_to_extF80M.c
    ${SF}/f32_to_f128.c
    ${SF}/f32_to_f128M.c
    ${SF}/f32_roundToInt.c
    ${SF}/f32_add.c
    ${SF}/f32_sub.c
    ${SF}/f32_mul.c
    ${SF}/f32_mulAdd.c
    ${SF}/f32_div.c
    ${SF}/f32_rem.c
    ${SF}/f32_sqrt.c
    ${SF}/f32_eq.c
    ${SF}/f32_le.c
    ${SF}/f32_lt.c
    ${SF}/f32_eq_signaling.c
    ${SF}/f32_le_quiet.c
    ${SF}/f32_lt_quiet.c
    ${SF}/f32_isSignalingNaN.c
    ${SF}/f64_to_ui32.c
    ${SF}/f64_to_ui64.c
    ${SF}/f64_to_i32.c
    ${SF}/f64_to_i64.c
    ${SF}/f64_to_ui32_r_minMag.c
    ${SF}/f64_to_ui64_r_minMag.c
    ${SF}/f64_to_i32_r_minMag.c
    ${SF}/f64_to_i64_r_minMag.c
    ${SF}/f64_to_f16.c
    ${SF}/f64_to_f32.c
    ${SF}/f64_to_extF80.c
    ${SF}/f64_to_extF80M.c
    ${SF}/f64_to_f128.c
    ${SF}/f64_to_f128M.c
    ${SF}/f64_roundToInt.c
    ${SF}/f64_add.c
    ${SF}/f64_sub.c
    ${SF}/f64_mul.c
    ${SF}/f64_mulAdd.c
    ${SF}/f64_div.c
    ${SF}/f64_rem.c
    ${SF}/f64_sqrt.c
    ${SF}/f64_eq.c
    ${SF}/f64_le.c
    ${SF}/f64_lt.c
    ${SF}/f64_eq_signaling.c
    ${SF}/f64_le_quiet.c
    ${SF}/f64_lt_quiet.c
    ${SF}/f64_isSignalingNaN.c
    ${SF}/extF80_to_ui32.c
    ${SF}/extF80_to_ui64.c
    ${SF}/extF80_to_i32.c
    ${SF}/extF80_to_i64.c
    ${SF}/extF80_to_ui32_r_minMag.c
    ${SF}/extF80_to_ui64_r_minMag.c
    ${SF}/extF80_to_i32_r_minMag.c
    ${SF}/extF80_to_i64_r_minMag.c
    ${SF}/extF80_to_f16.c
    ${SF}/extF80_to_f32.c
    ${SF}/extF80_to_f64.c
    ${SF}/extF80_to_f128.c
    ${SF}/extF80_roundToInt.c
    ${SF}/extF80_add.c
    ${SF}/extF80_sub.c
    ${SF}/extF80_mul.c
    ${SF}/extF80_div.c
    ${SF}/extF80_rem.c
    ${SF}/extF80_sqrt.c
    ${SF}/extF80_eq.c
    ${SF}/extF80_le.c
    ${SF}/extF80_lt.c
    ${SF}/extF80_eq_signaling.c
    ${SF}/extF80_le_quiet.c
    ${SF}/extF80_lt_quiet.c
    ${SF}/extF80_isSignalingNaN.c
    ${SF}/extF80M_to_ui32.c
    ${SF}/extF80M_to_ui64.c
    ${SF}/extF80M_to_i32.c
    ${SF}/extF80M_to_i64.c
    ${SF}/extF80M_to_ui32_r_minMag.c
    ${SF}/extF80M_to_ui64_r_minMag.c
    ${SF}/extF80M_to_i32_r_minMag.c
    ${SF}/extF80M_to_i64_r_minMag.c
    ${SF}/extF80M_to_f16.c
    ${SF}/extF80M_to_f32.c
    ${SF}/extF80M_to_f64.c
    ${SF}/extF80M_to_f128M.c
    ${SF}/extF80M_roundToInt.c
    ${SF}/extF80M_add.c
    ${SF}/extF80M_sub.c
    ${SF}/extF80M_mul.c
    ${SF}/extF80M_div.c
    ${SF}/extF80M_rem.c
    ${SF}/extF80M_sqrt.c
    ${SF}/extF80M_eq.c
    ${SF}/extF80M_le.c
    ${SF}/extF80M_lt.c
    ${SF}/extF80M_eq_signaling.c
    ${SF}/extF80M_le_quiet.c
    ${SF}/extF80M_lt_quiet.c
    ${SF}/f128_to_ui32.c
    ${SF}/f128_to_ui64.c
    ${SF}/f128_to_i32.c
    ${SF}/f128_to_i64.c
    ${SF}/f128_to_ui32_r_minMag.c
    ${SF}/f128_to_ui64_r_minMag.c
    ${SF}/f128_to_i32_r_minMag.c
    ${SF}/f128_to_i64_r_minMag.c
    ${SF}/f128_to_f16.c
    ${SF}/f128_to_f32.c
    ${SF}/f128_to_extF80.c
    ${SF}/f128_to_f64.c
    ${SF}/f128_roundToInt.c
    ${SF}/f128_add.c
    ${SF}/f128_sub.c
    ${SF}/f128_mul.c
    ${SF}/f128_mulAdd.c
    ${SF}/f128_div.c
    ${SF}/f128_rem.c
    ${SF}/f128_sqrt.c
    ${SF}/f128_eq.c
    ${SF}/f128_le.c
    ${SF}/f128_lt.c
    ${SF}/f128_eq_signaling.c
    ${SF}/f128_le_quiet.c
    ${SF}/f128_lt_quiet.c
    ${SF}/f128_isSignalingNaN.c
    ${SF}/f128M_to_ui32.c
    ${SF}/f128M_to_ui64.c
    ${SF}/f128M_to_i32.c
    ${SF}/f128M_to_i64.c
    ${SF}/f128M_to_ui32_r_minMag.c
    ${SF}/f128M_to_ui64_r_minMag.c
    ${SF}/f128M_to_i32_r_minMag.c
    ${SF}/f128M_to_i64_r_minMag.c
    ${SF}/f128M_to_f16.c
    ${SF}/f128M_to_f32.c
    ${SF}/f128M_to_extF80M.c
    ${SF}/f128M_to_f64.c
    ${SF}/f128M_roundToInt.c
    ${SF}/f128M_add.c
    ${SF}/f128M_sub.c
    ${SF}/f128M_mul.c
    ${SF}/f128M_mulAdd.c
    ${SF}/f128M_div.c
    ${SF}/f128M_rem.c
    ${SF}/f128M_sqrt.c
    ${SF}/f128M_eq.c
    ${SF}/f128M_le.c
    ${SF}/f128M_lt.c
    ${SF}/f128M_eq_signaling.c
    ${SF}/f128M_le_quiet.c
    ${SF}/f128M_lt_quiet.c)
add_library(softfloat STATIC ${SOFTFLOAT_SRC})
target_include_directories(softfloat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/softfloat
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/softfloat/source/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/softfloat/source/8086-SSE)
target_compile_definitions(softfloat PUBLIC LITTLEENDIAN=1 SOFTFLOAT_FAST_INT64 SOFTFLOAT_FAST_DIV64TO32 SOFTFLOAT_FAST_DIV32TO16 INLINE_LEVEL=5 SOFTFLOAT_ROUND_ODD)
target_compile_options(softfloat PRIVATE -O2 -w)
set_property(TARGET softfloat PROPERTY C_STANDARD 11)

# The core proper. Deliberately does NOT contain the golden vectors: difftest
# links this and regenerates those, so bundling them together makes broken
# vectors un-regenerable -- xcore fails to build, so difftest cannot be built
# to fix them.
add_library(xcore STATIC
    src/mem.c
    src/interp.c
    src/xop.c
    src/cache.c)
target_include_directories(xcore PUBLIC include)
target_link_libraries(xcore PUBLIC Zydis softfloat)
if(NOT APPLE)
    target_link_libraries(xcore PUBLIC m)
endif()
set_property(TARGET xcore PROPERTY C_STANDARD 11)
target_compile_options(xcore PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Recorded silicon post-states plus the replay harness. Linked by the device
# app and by test_selftest; never by difftest.
add_library(xcore_golden STATIC
    src/selftest.c
    src/golden_vectors.c)
target_link_libraries(xcore_golden PUBLIC xcore)
set_property(TARGET xcore_golden PROPERTY C_STANDARD 11)
target_compile_options(xcore_golden PRIVATE -Wall -Wextra -Wno-unused-parameter)
