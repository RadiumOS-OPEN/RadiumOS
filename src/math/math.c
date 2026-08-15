#include "math.h"

// ===== BASIC ARITHMETIC =====

int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

int mult(int a, int b) {
    return a * b;
}

int div(int a, int b) {
    if (b == 0) return 0; // Prevent division by zero
    return a / b;
}

int mod(int a, int b) {
    if (b == 0) return 0;
    return a % b;
}

// ===== POWER AND ROOT =====

int pow_int(int base, int exp) {
    if (exp < 0) return 0;
    if (exp == 0) return 1;
    
    int result = 1;
    for (int i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

double pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    
    // Handle negative exponents
    bool neg_exp = false;
    if (exp < 0) {
        neg_exp = true;
        exp = -exp;
    }
    
    // Use exp(exp * ln(base))
    double result = exp_func(exp * ln(base));
    
    if (neg_exp) {
        return 1.0 / result;
    }
    return result;
}

double sqrt(double x) {
    if (x < 0.0) return 0.0;
    if (x == 0.0) return 0.0;
    
    // Newton-Raphson method
    double guess = x / 2.0;
    double epsilon = 0.00001;
    
    for (int i = 0; i < 50; i++) {
        double new_guess = (guess + x / guess) / 2.0;
        if (fabs(new_guess - guess) < epsilon) {
            return new_guess;
        }
        guess = new_guess;
    }
    
    return guess;
}

// Integer square root
int isqrt(int x) {
    if (x < 0) return 0;
    if (x == 0) return 0;
    if (x == 1) return 1;
    
    int start = 1, end = x, ans = 0;
    
    while (start <= end) {
        int mid = (start + end) / 2;
        
        // Check if mid is perfect square
        if (mid <= x / mid) {
            start = mid + 1;
            ans = mid;
        } else {
            end = mid - 1;
        }
    }
    
    return ans;
}

// ===== ABSOLUTE VALUE =====


double fabs(double x) {
    return (x < 0.0) ? -x : x;
}

// ===== ROUNDING FUNCTIONS =====

double floor(double x) {
    int i = (int)x;
    if (x < 0.0 && x != (double)i) {
        return (double)(i - 1);
    }
    return (double)i;
}

double ceil(double x) {
    int i = (int)x;
    if (x > 0.0 && x != (double)i) {
        return (double)(i + 1);
    }
    return (double)i;
}

double round(double x) {
    if (x >= 0.0) {
        return floor(x + 0.5);
    } else {
        return ceil(x - 0.5);
    }
}

double trunc(double x) {
    return (double)((int)x);
}

// ===== EXPONENTIAL AND LOGARITHM =====

double exp_func(double x) {
    // Taylor series: e^x = 1 + x + x^2/2! + x^3/3! + ...
    double result = 1.0;
    double term = 1.0;
    
    for (int i = 1; i < 100; i++) {
        term *= x / i;
        result += term;
        
        if (fabs(term) < 0.00000001) {
            break;
        }
    }
    
    return result;
}

double ln(double x) {
    if (x <= 0.0) return 0.0;
    if (x == 1.0) return 0.0;
    
    // Use Newton's method: ln(x) = y where e^y = x
    double y = 0.0;
    double epsilon = 0.00001;
    
    for (int i = 0; i < 100; i++) {
        double ey = exp_func(y);
        double dy = (x - ey) / ey;
        y += dy;
        
        if (fabs(dy) < epsilon) {
            break;
        }
    }
    
    return y;
}

double log10(double x) {
    return ln(x) / LN10;
}

double log2(double x) {
    return ln(x) / LN2;
}

// ===== TRIGONOMETRIC FUNCTIONS =====

// Normalize angle to [-PI, PI]
double normalize_angle(double x) {
    while (x > PI) {
        x -= 2.0 * PI;
    }
    while (x < -PI) {
        x += 2.0 * PI;
    }
    return x;
}

double sin(double x) {
    // Normalize angle
    x = normalize_angle(x);
    
    // Taylor series: sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ...
    double result = 0.0;
    double term = x;
    double x_squared = x * x;
    
    for (int i = 0; i < 20; i++) {
        result += term;
        term *= -x_squared / ((2 * i + 2) * (2 * i + 3));
        
        if (fabs(term) < 0.00000001) {
            break;
        }
    }
    
    return result;
}

double cos(double x) {
    // Normalize angle
    x = normalize_angle(x);
    
    // Taylor series: cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + ...
    double result = 0.0;
    double term = 1.0;
    double x_squared = x * x;
    
    for (int i = 0; i < 20; i++) {
        result += term;
        term *= -x_squared / ((2 * i + 1) * (2 * i + 2));
        
        if (fabs(term) < 0.00000001) {
            break;
        }
    }
    
    return result;
}

double tan(double x) {
    double cos_x = cos(x);
    if (fabs(cos_x) < 0.00001) {
        return 0.0; // Undefined, return 0
    }
    return sin(x) / cos_x;
}

// ===== INVERSE TRIGONOMETRIC FUNCTIONS =====

double asin(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    
    // Use Taylor series for small values
    if (fabs(x) < 0.5) {
        double result = x;
        double term = x;
        double x_squared = x * x;
        
        for (int n = 1; n < 20; n++) {
            term *= x_squared * (2 * n - 1) * (2 * n - 1) / ((2 * n) * (2 * n + 1));
            result += term;
            
            if (fabs(term) < 0.00000001) {
                break;
            }
        }
        
        return result;
    }
    
    // For larger values, use: asin(x) = PI/2 - asin(sqrt(1-x^2))
    if (x > 0) {
        return PI / 2.0 - asin(sqrt(1.0 - x * x));
    } else {
        return -PI / 2.0 + asin(sqrt(1.0 - x * x));
    }
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return PI / 2.0 - asin(x);
}

double atan(double x) {
    // Use Taylor series for |x| < 1
    if (fabs(x) < 1.0) {
        double result = 0.0;
        double term = x;
        double x_squared = x * x;
        
        for (int n = 0; n < 50; n++) {
            result += term / (2 * n + 1);
            term *= -x_squared;
            
            if (fabs(term / (2 * n + 3)) < 0.00000001) {
                break;
            }
        }
        
        return result;
    }
    
    // For |x| >= 1, use: atan(x) = PI/2 - atan(1/x) for x > 0
    if (x > 0) {
        return PI / 2.0 - atan(1.0 / x);
    } else {
        return -PI / 2.0 - atan(1.0 / x);
    }
}

double atan2(double y, double x) {
    if (x > 0) {
        return atan(y / x);
    } else if (x < 0 && y >= 0) {
        return atan(y / x) + PI;
    } else if (x < 0 && y < 0) {
        return atan(y / x) - PI;
    } else if (x == 0 && y > 0) {
        return PI / 2.0;
    } else if (x == 0 && y < 0) {
        return -PI / 2.0;
    }
    return 0.0; // x == 0 && y == 0
}

// ===== HYPERBOLIC FUNCTIONS =====

double sinh(double x) {
    // sinh(x) = (e^x - e^-x) / 2
    double ex = exp_func(x);
    return (ex - 1.0 / ex) / 2.0;
}

double cosh(double x) {
    // cosh(x) = (e^x + e^-x) / 2
    double ex = exp_func(x);
    return (ex + 1.0 / ex) / 2.0;
}

double tanh(double x) {
    // tanh(x) = sinh(x) / cosh(x)
    double ex = exp_func(x);
    double emx = 1.0 / ex;
    return (ex - emx) / (ex + emx);
}

// ===== UTILITY FUNCTIONS =====

int min(int a, int b) {
    return (a < b) ? a : b;
}

int max(int a, int b) {
    return (a > b) ? a : b;
}

double fmin(double a, double b) {
    return (a < b) ? a : b;
}

double fmax(double a, double b) {
    return (a > b) ? a : b;
}

int clamp(int x, int min_val, int max_val) {
    if (x < min_val) return min_val;
    if (x > max_val) return max_val;
    return x;
}

double fclamp(double x, double min_val, double max_val) {
    if (x < min_val) return min_val;
    if (x > max_val) return max_val;
    return x;
}

// Linear interpolation
double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// Map value from one range to another
double map_range(double x, double in_min, double in_max, double out_min, double out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Degrees to radians
double deg_to_rad(double degrees) {
    return degrees * PI / 180.0;
}

// Radians to degrees
double rad_to_deg(double radians) {
    return radians * 180.0 / PI;
}

// Sign function
int sign(int x) {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
}

double fsign(double x) {
    if (x > 0.0) return 1.0;
    if (x < 0.0) return -1.0;
    return 0.0;
}

// GCD (Greatest Common Divisor)
int gcd(int a, int b) {
    a = abs(a);
    b = abs(b);
    
    while (b != 0) {
        int temp = b;
        b = a % b;
        a = temp;
    }
    
    return a;
}

// LCM (Least Common Multiple)
int lcm(int a, int b) {
    if (a == 0 || b == 0) return 0;
    return abs(a * b) / gcd(a, b);
}

// Factorial
int factorial(int n) {
    if (n < 0) return 0;
    if (n == 0 || n == 1) return 1;
    
    int result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    
    return result;
}

// Fibonacci
int fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    
    int a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        int temp = a + b;
        a = b;
        b = temp;
    }
    
    return b;
}

// Check if prime
bool is_prime(int n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    
    for (int i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) {
            return false;
        }
    }
    
    return true;
}




double frand(void) {
    return (double)rand() / (double)0x7FFFFFFF;
}

double frand_range(double min, double max) {
    return min + frand() * (max - min);
}