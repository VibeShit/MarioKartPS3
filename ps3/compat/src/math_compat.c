/* See compat/include/math.h. */
#include <math.h>

long double log2l(long double x) { return log2((double)x); }
long double logbl(long double x) { return logb((double)x); }
double nexttoward(double x, long double y) { return nextafter(x, (double)y); }
float nexttowardf(float x, long double y) { return nextafterf(x, (float)y); }
long double nexttowardl(long double x, long double y) { return nextafter((double)x, (double)y); }
