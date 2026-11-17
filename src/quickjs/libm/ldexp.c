#include "libm.h"
#include <math.h>

double ldexp(double x, int n)
{
	return scalbn(x, n);
}
