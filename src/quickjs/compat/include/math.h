/* math.h for QuickJS on RadiumOS: musl libm (src/quickjs/libm), renamed to
   qjs_* so it never collides with the kernel's own math.c. */
#ifndef QJS_MATH_H
#define QJS_MATH_H

#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define isnormal(x) __builtin_isnormal(x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

/* x87: intermediates are long double unless stored (FLT_EVAL_METHOD 2) */
typedef long double double_t;
typedef long double float_t;

#define acos qjs_acos
double qjs_acos(double x);
#define asin qjs_asin
double qjs_asin(double x);
#define atan qjs_atan
double qjs_atan(double x);
#define cos qjs_cos
double qjs_cos(double x);
#define sin qjs_sin
double qjs_sin(double x);
#define tan qjs_tan
double qjs_tan(double x);
#define acosh qjs_acosh
double qjs_acosh(double x);
#define asinh qjs_asinh
double qjs_asinh(double x);
#define atanh qjs_atanh
double qjs_atanh(double x);
#define cosh qjs_cosh
double qjs_cosh(double x);
#define sinh qjs_sinh
double qjs_sinh(double x);
#define tanh qjs_tanh
double qjs_tanh(double x);
#define exp qjs_exp
double qjs_exp(double x);
#define exp2 qjs_exp2
double qjs_exp2(double x);
#define expm1 qjs_expm1
double qjs_expm1(double x);
#define log qjs_log
double qjs_log(double x);
#define log10 qjs_log10
double qjs_log10(double x);
#define log1p qjs_log1p
double qjs_log1p(double x);
#define log2 qjs_log2
double qjs_log2(double x);
#define cbrt qjs_cbrt
double qjs_cbrt(double x);
#define sqrt qjs_sqrt
double qjs_sqrt(double x);
#define fabs qjs_fabs
double qjs_fabs(double x);
#define ceil qjs_ceil
double qjs_ceil(double x);
#define floor qjs_floor
double qjs_floor(double x);
#define trunc qjs_trunc
double qjs_trunc(double x);
#define round qjs_round
double qjs_round(double x);
#define rint qjs_rint
double qjs_rint(double x);
#define nearbyint qjs_nearbyint
double qjs_nearbyint(double x);
#define atan2 qjs_atan2
double qjs_atan2(double x, double y);
#define pow qjs_pow
double qjs_pow(double x, double y);
#define hypot qjs_hypot
double qjs_hypot(double x, double y);
#define fmod qjs_fmod
double qjs_fmod(double x, double y);
#define fmin qjs_fmin
double qjs_fmin(double x, double y);
#define fmax qjs_fmax
double qjs_fmax(double x, double y);
#define copysign qjs_copysign
double qjs_copysign(double x, double y);
#define remainder qjs_remainder
double qjs_remainder(double x, double y);
#define lrint qjs_lrint
long qjs_lrint(double x);
#define llrint qjs_llrint
long long qjs_llrint(double x);
#define scalbn qjs_scalbn
double qjs_scalbn(double x, int n);
#define ldexp qjs_ldexp
double qjs_ldexp(double x, int n);
#define frexp qjs_frexp
double qjs_frexp(double x, int *e);
#define modf qjs_modf
double qjs_modf(double x, double *ip);
#define fabsf qjs_fabsf
float qjs_fabsf(float x);

#endif
