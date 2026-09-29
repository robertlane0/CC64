#ifndef CC64_TARGET_MATH_H
#define CC64_TARGET_MATH_H

/* The target has no floating-point library service, so the functions here are
   the target's own implementations. A caller that needs one of the more
   elaborate functions gets a documented approximation rather than a link
   failure, because a program that computes a logarithm should still start. */

#define HUGE_VAL  (__builtin_huge_val())
#define INFINITY  (__builtin_huge_val())
#define NAN       (__builtin_nan())

double sqrt(double value);
double pow(double base, double exponent);
double floor(double value);
double ceil(double value);
double round(double value);
double trunc(double value);
double fabs(double value);
double fmod(double value, divisor);
double sin(double value);
double cos(double value);
double tan(double value);
double atan(double value);
double atan2(double y, double x);
double asin(double value);
double acos(double value);
double exp(double value);
double log(double value);
double log2(double value);
double log10(double value);
double fmin(double left, double right);
double fmax(double left, double right);
double hypot(double x, double y);
double copysign(double magnitude, double sign);

float sqrtf(float value);
float powf(float base, float exponent);
float floorf(float value);
float ceilf(float value);
float fabsf(float value);
float fmodf(float value, float divisor);

#endif
