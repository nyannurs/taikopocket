#ifndef TP_POCKET_MATH_H
#define TP_POCKET_MATH_H
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline double fmin(double a, double b) { return a < b ? a : b; }
static inline double fmax(double a, double b) { return a > b ? a : b; }
double floor(double x);
double ceil(double x);
double sin(double x);
double cos(double x);
long long llround(double x);
#endif
