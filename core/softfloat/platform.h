/* SoftFloat platform header for xcore: little-endian hosts (x86-64, arm64),
 * GCC or clang. See third_party/softfloat/build/Linux-x86_64-GCC/platform.h
 * for the original this is condensed from. */
#define LITTLEENDIAN 1
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"
