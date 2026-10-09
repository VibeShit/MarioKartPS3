/* Force-included (-include) into every PS3 translation unit.
 *
 * newlib omits a few C99 long-double/nexttoward functions that libstdc++'s
 * <cmath> imports into std when _GLIBCXX_USE_C99_MATH_TR1 is enabled. long
 * double is 64-bit on the PPU, so they are thin forwards defined in
 * compat/src/math_compat.c. <cmath> reaches newlib's <math.h> through
 * #include_next, which is why these cannot live in a math.h wrapper. */
#ifndef MKW_PS3_PRELUDE_H
#define MKW_PS3_PRELUDE_H
#ifdef __cplusplus
extern "C" {
#endif
long double log2l(long double x);
long double logbl(long double x);
double nexttoward(double x, long double y);
float nexttowardf(float x, long double y);
long double nexttowardl(long double x, long double y);
#ifdef __cplusplus
}
#endif

/* newlib only typedefs float_t/double_t when FLT_EVAL_METHOD is still
 * undefined, i.e. when <math.h> comes before <float.h>; libstdc++'s <cmath>
 * then needs them. Pull <math.h> in first so every TU sees them. */
#include <math.h>
#endif
