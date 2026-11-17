/* sqrt for RadiumOS QuickJS libm.
 *
 * Replaces musl's soft-float src/math/sqrt.c: on x87 the hardware fsqrt is
 * correctly rounded (to the FPU's current precision, 53 bits in the kernel).
 * Other targets (host tests) use the compiler builtin. */
#include "libm.h"

double sqrt(double x)
{
#if defined(__i386__)
	double r;
	__asm__ ("fsqrt" : "=t"(r) : "0"(x));
	return r;
#else
	return __builtin_sqrt(x);
#endif
}
