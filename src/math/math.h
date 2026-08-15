#ifndef MATH_H
#define MATH_H

#include <stdbool.h>

// Constants
#define PI 3.14159265358979323846
#define E  2.71828182845904523536
#define LN2 0.69314718055994530942
#define LN10 2.30258509299404568402

// Basic arithmetic
int add(int a, int b);
int sub(int a, int b);
int mult(int a, int b);
int div(int a, int b);
int mod(int a, int b);

// Power and root
int pow_int(int base, int exp);
double pow(double base, double exp);
double sqrt(double x);
int isqrt(int x);

// Absolute value
int abs(int x);
double fabs(double x);

// Rounding
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);

// Exponential and logarithm
double exp_func(double x);
double ln(double x);
double log10(double x);
double log2(double x);

// Trigonometric
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

// Hyperbolic
double sinh(double x);
double cosh(double x);
double tanh(double x);

// Utility
int min(int a, int b);
int max(int a, int b);
double fmin(double a, double b);
double fmax(double a, double b);
int clamp(int x, int min_val, int max_val);
double fclamp(double x, double min_val, double max_val);
double lerp(double a, double b, double t);
double map_range(double x, double in_min, double in_max, double out_min, double out_max);
double deg_to_rad(double degrees);
double rad_to_deg(double radians);
int sign(int x);
double fsign(double x);

// Number theory
int gcd(int a, int b);
int lcm(int a, int b);
int factorial(int n);
int fibonacci(int n);
bool is_prime(int n);

// Random
void srand(unsigned int seed);
int rand(void);
int rand_range(int min, int max);
double frand(void);
double frand_range(double min, double max);

#endif // MATH_H