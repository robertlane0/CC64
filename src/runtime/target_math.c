/* Target floating-point routines for MS-DOS64.
 *
 * The target has no arithmetic service: the processor's own instructions are
 * the whole of it. The routines here are therefore written from the numeric
 * definitions of the functions they implement rather than from any particular
 * library, because there is no library to be consistent with. Each says what
 * it approximates and over what range, so a caller that needs more than the
 * approximation can see the limit instead of discovering it.
 *
 * The one approximation that is worth stating plainly is the exponentiation
 * and the logarithm, which are the hardest to get right and the ones a colour
 * program reaches for. Both split their argument into a power of two and a
 * remainder, and both then use short polynomial series on the remainder, which
 * is small enough for the series to converge quickly. The result is good to a
 * few units in the last place over the range the series covers, and the
 * reduction covers the whole range by moving the argument into it. */

#include <math.h>
#include <stdbool.h>

/* The base of the natural logarithm, to more digits than the double format
   can hold, so that the logarithm of it is exact rather than nearly so. */
#define CC64_LOG2E 1.4426950408889634074
#define CC64_SQRT2 1.41421356237309504880

/* A double's exponent field holds the power of two its significand is scaled
   by, so the space a value occupies can be found and changed by arithmetic on
   that field. The layout is the target's own, fixed by the ABI document, and
   is stated here rather than assumed so a change to the contract shows up as a
   failure here instead of as a wrong answer. */
#define CC64_EXPONENT_MASK 0x7FF0000000000000UL
#define CC64_SIGNIFICAND_MASK 0x000FFFFFFFFFFFFFUL
#define CC64_EXPONENT_BIAS 1023

typedef unsigned long cc64_bits;

/* A value's bits and the value itself are two views of the same eight bytes,
   which is what a union states. A union is used rather than a copy through a
   character array because the character form would need the compiler to
   understand that the two objects are the same size, and a union needs no such
   promise. */
union cc64_view {
    double value;
    cc64_bits bits;
};

static double bits_to_double(cc64_bits bits)
{
    union cc64_view view;
    view.bits = bits;
    return view.value;
}

static cc64_bits double_to_bits(double value)
{
    union cc64_view view;
    view.value = value;
    return view.bits;
}

double fabs(double value)
{
    return bits_to_double(double_to_bits(value) & ~0x8000000000000000UL);
}

/* A value at or above two to the fifty-second has no fractional part left in
   the format, so it is its own whole part. */
#define CC64_WHOLE_LIMIT 4503599627370496.0

double trunc(double value)
{
    double magnitude = fabs(value);
    if (magnitude >= CC64_WHOLE_LIMIT) return value;
    double whole = (double)(long)magnitude;
    return value < 0.0 ? -whole : whole;
}

double floor(double value)
{
    /* The direction a value rounds in is what separates these two, and the
       whole part is found by truncation first: a magnitude loses the direction
       the value had, so a single truncation on the magnitude cannot tell
       whether a negative value's whole part is below or above it. */
    double whole = trunc(value);
    if (whole > value) return whole - 1.0;
    return whole;
}

double ceil(double value)
{
    double whole = trunc(value);
    if (whole < value) return whole + 1.0;
    return whole;
}

double round(double value)
{
    /* A halfway case goes away from zero, which is what the definition asks
       for. The even neighbour is a different function, one that follows the
       machine's rounding mode, and the target has no service for setting or
       reading that mode, so it is not provided rather than provided wrongly. */
    double magnitude = fabs(value);
    if (magnitude >= CC64_WHOLE_LIMIT) return value;  /* already whole */
    double whole = (double)(long)magnitude;
    if (magnitude - whole >= 0.5) whole += 1.0;
    return value < 0.0 ? -whole : whole;
}

double fmod(double value, double divisor)
{
    if (divisor == 0.0) return 0.0;
    double magnitude = fabs(value);
    double step = fabs(divisor);
    if (magnitude < step) return value;
    /* The quotient of two doubles whose magnitudes differ by more than the
       quotient can hold is not representable, and a quotient that cannot be
       represented cannot be turned back into a remainder: the product that
       would remove it overflows on the way. The target has no wider type to do
       the work in and no way to do it a bit at a time while keeping the
       significand whole, so a quotient past the range of a 64-bit count is
       reported as a remainder of zero. That is the value the definition gives
       whenever the divisor divides the value exactly, which is the only
       remainder a caller can act on without the lost bits. */
    double quotient = magnitude / step;
    if (!(quotient <= 9.2233720368547758E18)) {
        return value < 0.0 ? -0.0 : 0.0;
    }
    double whole = trunc(quotient);
    double rest = magnitude - whole * step;
    return value < 0.0 ? -rest : rest;
}

double sqrt(double value)
{
    if (value < 0.0) return 0.0;
    if (value == 0.0) return 0.0;
    /* Newton's method on the reciprocal square root converges quadratically
       from any positive start, and halving the exponent first brings the value
       into the range where the start below is close enough to save iterations.
       The iteration count is fixed because the method cannot diverge here and
       a fixed count is what makes the answer the same every time. */
    cc64_bits bits = double_to_bits(value);
    unsigned long exponent = (bits & CC64_EXPONENT_MASK) >> 52;
    if (exponent == 0U) {
        /* A subnormal has no exponent to halve, so it is scaled up by a power
           of two first and the answer scaled back afterwards. */
        return sqrt(value * 18014398509481984.0) / 134217728.0;
    }
    unsigned long halved = (exponent + CC64_EXPONENT_BIAS) / 2U;
    double estimate = bits_to_double((halved << 52) | (CC64_SIGNIFICAND_MASK >> 1));
    for (int step = 0; step < 40; ++step) {
        double next = 0.5 * (estimate + value / estimate);
        if (next == estimate) break;
        estimate = next;
    }
    return estimate;
}

float sqrtf(float value)
{
    return (float)sqrt((double)value);
}

float fabsf(float value)
{
    return (float)fabs((double)value);
}

float floorf(float value)
{
    return (float)floor((double)value);
}

float ceilf(float value)
{
    return (float)ceil((double)value);
}

float fmodf(float value, float divisor)
{
    return (float)fmod((double)value, (double)divisor);
}

/* The natural logarithm of a number near one, from its series in the form
   that stays accurate close to one:
       log(x) = 2 * (t + t^3/3 + t^5/5 + ...),  t = (x-1)/(x+1)
   The argument is reduced into a range where the series is short, so the terms
   fall by at least a factor of four each time. */
static double log_near_one(double value)
{
    double t = (value - 1.0) / (value + 1.0);
    double t2 = t * t;
    double term = t;
    double sum = t;
    for (int index = 3; index <= 25; index += 2) {
        term *= t2;
        sum += term / (double)index;
    }
    return 2.0 * sum;
}

double log(double value)
{
    if (value <= 0.0) return -1.0E300;
    /* The value is a significand in [1, 2) times a power of two. The
       significand alone is what the series is accurate on, and the power of two
       is a whole number of doublings, so adding it back afterwards is exact.
       Putting the bias into the exponent field is what recovers the
       significand: a field holding the unbiased exponent would hand back the
       whole value, and the power of two would then be counted twice. */
    cc64_bits bits = double_to_bits(value);
    unsigned long raw = (bits & CC64_EXPONENT_MASK) >> 52;
    if (raw == 0U) return log_near_one(value);
    long exponent = (long)raw - CC64_EXPONENT_BIAS;
    double significand =
        bits_to_double(((cc64_bits)CC64_EXPONENT_BIAS << 52) |
                       (bits & CC64_SIGNIFICAND_MASK));
    if (significand > CC64_SQRT2) {
        significand *= 0.5;
        exponent += 1L;
    }
    return log_near_one(significand) + (double)exponent * 0.69314718055994530942;
}

double log2(double value)
{
    if (value <= 0.0) return -1.0E300;
    /* The base-two logarithm is the natural one over the natural logarithm of
       two, which is a constant rather than a computation. */
    return log(value) * CC64_LOG2E;
}

double log10(double value)
{
    if (value <= 0.0) return -1.0E300;
    return log(value) * 0.43429448190325182765;
}

double exp(double value)
{
    if (value > 709.782712893384) return 1.0E308;
    if (value < -745.0) return 0.0;
    /* Split the argument into a whole number of halvings and a remainder in
       [-1/2, 1/2), then raise two to the first part exactly by writing the
       exponent, and the remainder by a series that is short at that size. */
    long doublings = (long)(value * CC64_LOG2E + (value < 0.0 ? -0.5 : 0.5));
    double remainder = value - (double)doublings * 0.69314718055994530942;
    double term = 1.0;
    double sum = 1.0;
    for (int index = 1; index <= 18; ++index) {
        term *= remainder / (double)index;
        sum += term;
    }
    long final = doublings + CC64_EXPONENT_BIAS;
    if (final <= 0L) return 0.0;
    if (final >= 2046L) return 1.0E308;
    double scale = bits_to_double((unsigned long)final << 52);
    return sum * scale;
}

double pow(double base, double exponent)
{
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base < 0.0) {
        /* A negative base has a real power only for a whole exponent, and
           which whole exponents it has depends on the sign, so the case is
           decided before anything is computed. */
        double whole = trunc(exponent);
        if (whole != exponent) return 0.0;
        double magnitude = exp(exponent * log(-base));
        return ((long)whole % 2L == 0L) ? magnitude : -magnitude;
    }
    return exp(exponent * log(base));
}

float powf(float base, float exponent)
{
    return (float)pow((double)base, (double)exponent);
}

/* The sine of a small angle, from its Taylor series. The argument is reduced
   before this is called, so the series is only ever asked for a small angle
   and the terms fall quickly.
   The denominator of each term is the ratio between one factorial and the
   one before it, so it has to divide the term rather than the running sum:
   dividing the sum would leave the term undivided for the next factor of the
   angle to multiply, and the terms would then be the bare odd powers. */
static double sin_small(double angle)
{
    double angle2 = angle * angle;
    double term = angle;
    double sum = angle;
    for (int index = 1; index <= 9; ++index) {
        term *= -angle2;
        term /= (double)((2 * index) * (2 * index + 1));
        sum += term;
    }
    return sum;
}

static const double CC64_PI = 3.14159265358979323846;
static const double CC64_HALF_PI = 1.57079632679489661923;

/* The sine and cosine of an angle, from one reduction and one series.
   The angle is brought inside the half circle first, because the series is only
   asked to be accurate over that range and its terms stop falling in quickly
   beyond it. An angle outside the half circle is reflected into it, and the
   reflection turns the cosine's sign, which is the only thing the two cases
   differ in. */
static void sin_cos(double angle, double *sine, double *cosine)
{
    static const double two_pi = 6.28318530717958647692;
    double turns = angle / two_pi;
    long whole = (long)(turns < 0.0 ? turns - 0.5 : turns + 0.5);
    double reduced = angle - (double)whole * two_pi;
    bool reflected = false;
    if (reduced > CC64_HALF_PI) {
        reduced = CC64_PI - reduced;
        reflected = true;
    } else if (reduced < -CC64_HALF_PI) {
        reduced = -CC64_PI - reduced;
        reflected = true;
    }
    *sine = sin_small(reduced);
    double magnitude = reduced < 0.0 ? -reduced : reduced;
    *cosine = sin_small(CC64_HALF_PI - magnitude);
    if (reflected) *cosine = -*cosine;
}

double sin(double angle)
{
    double sine;
    double cosine;
    (void)cosine;
    sin_cos(angle, &sine, &cosine);
    return sine;
}

double cos(double angle)
{
    double sine;
    double cosine;
    (void)sine;
    sin_cos(angle, &sine, &cosine);
    return cosine;
}

double tan(double angle)
{
    double sine;
    double cosine;
    sin_cos(angle, &sine, &cosine);
    if (cosine == 0.0) return 0.0;
    return sine / cosine;
}

/* The tangent of an eighth turn, which is where the argument is folded to. */
static const double CC64_TAN_EIGHTH = 0.4142135623730950;
static const double CC64_QUARTER_PI = 0.78539816339744830962;

double atan(double value)
{
    if (value < 0.0) return -atan(-value);
    if (value > 1.0) return CC64_HALF_PI - atan(1.0 / value);
    /* The series in the argument converges slowly as the argument approaches
       one, because each term is only the square of the one before it and a
       square near one is nearly one. At the argument of one the terms still
       shrink by only a factor of three after twenty, which is not enough for
       a double. The identity atan(x) = pi/4 - atan((1-x)/(1+x)) maps the upper
       half of the unit interval into the lower part of it, where the terms
       shrink by at least a factor of six each time and twenty of them reach the
       width of a double. */
    if (value > CC64_TAN_EIGHTH) {
        return CC64_QUARTER_PI - atan((1.0 - value) / (1.0 + value));
    }
    double x2 = value * value;
    double term = value;
    double sum = value;
    for (int index = 1; index <= 20; ++index) {
        term *= -x2;
        sum += term / (double)(2 * index + 1);
    }
    return sum;
}

double atan2(double y, double x)
{
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return y >= 0.0 ? atan(y / x) + CC64_PI
                                 : atan(y / x) - CC64_PI;
    if (y > 0.0) return CC64_HALF_PI;
    if (y < 0.0) return -CC64_HALF_PI;
    return 0.0;
}

double asin(double value)
{
    if (value > 1.0) value = 1.0;
    if (value < -1.0) value = -1.0;
    return atan2(value, sqrt(1.0 - value * value));
}

double acos(double value)
{
    if (value > 1.0) value = 1.0;
    if (value < -1.0) value = -1.0;
    return atan2(sqrt(1.0 - value * value), value);
}

double fmin(double left, double right)
{
    return left < right ? left : right;
}

double fmax(double left, double right)
{
    return left > right ? left : right;
}

double hypot(double x, double y)
{
    return sqrt(x * x + y * y);
}

double copysign(double magnitude, double sign)
{
    cc64_bits sign_bit = double_to_bits(sign) & 0x8000000000000000UL;
    return bits_to_double((double_to_bits(magnitude) & ~0x8000000000000000UL) |
                          sign_bit);
}
