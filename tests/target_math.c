/* The target's own arithmetic routines, checked against values worked out from
   their definitions rather than against another library.
 *
 * The target has no arithmetic service, so these routines are the whole of the
   floating-point support a program has. Each check states the identity the
   function must satisfy and the range it must hold it over, so a routine that
 * is merely close fails rather than passing a comparison against a value some
 * other implementation produced. */

#include <math.h>

int cc64_write(int, const void *, unsigned long);

/* A relative comparison, because a function's answer is only meaningful
   relative to the size of the answer. The tolerance is loose enough to allow
   the working precision the conversions use and tight enough to fail a
   routine that computes a different function. */
static int near(double got, double want, double tolerance)
{
    double difference = got - want;
    if (difference < 0.0) difference = -difference;
    double scale = want < 0.0 ? -want : want;
    if (scale < 1.0) scale = 1.0;
    return difference <= scale * tolerance;
}

int main(void)
{
    /* A square root is its own argument's square back. */
    if (!near(sqrt(0.0), 0.0, 0.0)) return 1;
    if (!near(sqrt(1.0), 1.0, 1e-15)) return 2;
    if (!near(sqrt(2.0), 1.4142135623730951, 1e-15)) return 3;
    if (!near(sqrt(1e12), 1000000.0, 1e-15)) return 4;
    if (!near(sqrt(1e-12), 1e-6, 1e-15)) return 5;
    /* A subnormal has no exponent to halve, so it takes the scaled path; the
       smallest normal is the boundary between the two. */
    if (!near(sqrt(1e-308), 1e-154, 1e-14)) return 6;
    if (!near(sqrt(4.9406564584124654e-324), 2.2227587494850775e-162, 1e-12)) return 7;
    if (!near(sqrt(1.7976931348623157e308), 1.3407807929942596e154, 1e-15)) return 8;

    /* A logarithm is the exponent that undoes an exponential. */
    if (!near(log(1.0), 0.0, 1e-15)) return 9;
    if (!near(log(2.0), 0.6931471805599453, 1e-15)) return 10;
    if (!near(log(10.0), 2.302585092994046, 1e-15)) return 11;
    if (!near(log(1e-300), -690.7755278982137, 1e-14)) return 12;
    if (!near(log(1e300), 690.7755278982137, 1e-14)) return 13;
    if (!near(log2(1024.0), 10.0, 1e-15)) return 14;
    if (!near(log2(1e300), 996.5784284662087, 1e-14)) return 15;
    if (!near(log10(1000.0), 3.0, 1e-15)) return 16;

    /* An exponential is the inverse of a logarithm. */
    if (!near(exp(0.0), 1.0, 1e-15)) return 17;
    if (!near(exp(1.0), 2.718281828459045, 1e-14)) return 18;
    if (!near(exp(-1.0), 0.36787944117144233, 1e-14)) return 19;
    if (!near(exp(10.0), 22026.465794806718, 1e-14)) return 20;
    if (!near(exp(-700.0), 9.859676543759777e-305, 1e-13)) return 21;

    /* A power is a logarithm and an exponential composed. */
    if (!near(pow(2.0, 10.0), 1024.0, 1e-14)) return 22;
    if (!near(pow(10.0, 3.0), 1000.0, 1e-14)) return 23;
    if (!near(pow(2.0, 0.5), sqrt(2.0), 1e-15)) return 24;
    if (!near(pow(3.0, -2.0), 1.0 / 9.0, 1e-14)) return 25;
    if (!near(pow(0.5, 0.0), 1.0, 1e-15)) return 26;
    /* A negative base has a real power only for a whole exponent, and which
       whole exponents depends on the sign. */
    if (!near(pow(-2.0, 3.0), -8.0, 1e-14)) return 27;
    if (!near(pow(-2.0, 4.0), 16.0, 1e-14)) return 28;
    if (pow(-2.0, 0.5) != 0.0) return 29;

    /* The sine and cosine of the quarter turn, and the identity between them. */
    if (!near(sin(0.0), 0.0, 1e-15)) return 30;
    if (!near(sin(1.5707963267948966), 1.0, 1e-15)) return 31;
    if (!near(sin(3.141592653589793), 0.0, 1e-15)) return 32;
    if (!near(sin(-1.0), -0.8414709848078965, 1e-14)) return 33;
    if (!near(cos(0.0), 1.0, 1e-15)) return 34;
    if (!near(cos(1.5707963267948966), 0.0, 1e-15)) return 35;
    if (!near(cos(3.141592653589793), -1.0, 1e-15)) return 36;
    if (!near(cos(-1.0), 0.5403023058681398, 1e-14)) return 37;
    /* An angle outside the half circle is where a series stops converging
       quickly, so both are checked there and against each other. */
    if (!near(sin(3.0), 0.1411200080598672, 1e-14)) return 38;
    if (!near(cos(3.0), -0.9899924966004454, 1e-14)) return 39;
    if (!near(sin(-3.0), -0.1411200080598672, 1e-14)) return 40;
    if (!near(cos(-3.0), -0.9899924966004454, 1e-14)) return 41;
    if (!near(sin(4.5), -0.9775301176650970, 1e-14)) return 42;
    if (!near(cos(4.5), -0.2107957994307797, 1e-14)) return 43;
    for (int step = 0; step <= 40; ++step) {
        double angle = -6.0 + (double)step * 0.3;
        if (!near(sin(angle) * sin(angle) + cos(angle) * cos(angle), 1.0, 1e-13)) {
            return 44;
        }
    }
    if (!near(tan(1.0), 1.5574077246549023, 1e-13)) return 45;

    /* The arc tangent of the two coordinate cases it is defined for. */
    if (!near(atan(0.0), 0.0, 1e-15)) return 46;
    if (!near(atan(1.0), 0.7853981633974483, 1e-15)) return 47;
    if (!near(atan(-1.0), -0.7853981633974483, 1e-15)) return 48;
    if (!near(atan(1e6), 1.5707953267948966, 1e-14)) return 49;
    if (!near(atan2(1.0, 1.0), 0.7853981633974483, 1e-15)) return 50;
    if (!near(atan2(1.0, -1.0), 2.356194490192345, 1e-15)) return 51;
    if (!near(atan2(-1.0, -1.0), -2.356194490192345, 1e-15)) return 52;
    if (!near(atan2(1.0, 0.0), 1.5707963267948966, 1e-15)) return 53;
    if (!near(asin(1.0), 1.5707963267948966, 1e-15)) return 54;
    if (!near(asin(0.0), 0.0, 1e-15)) return 55;
    if (!near(acos(1.0), 0.0, 1e-15)) return 56;
    if (!near(acos(0.0), 1.5707963267948966, 1e-15)) return 57;

    /* The rounding family, at and around the halfway cases. A halfway case
       goes away from zero, which is what the definition asks for; the even
       neighbour belongs to the function that follows the machine's rounding
       mode, which the target cannot expose. */
    if (floor(1.5) != 1.0 || floor(-1.5) != -2.0) return 58;
    if (ceil(1.5) != 2.0 || ceil(-1.5) != -1.0) return 59;
    if (trunc(1.9) != 1.0 || trunc(-1.9) != -1.0) return 60;
    if (round(0.5) != 1.0 || round(1.5) != 2.0 || round(2.5) != 3.0) return 61;
    if (round(-0.5) != -1.0 || round(-1.5) != -2.0 || round(-2.5) != -3.0) return 62;
    if (round(2.4) != 2.0 || round(2.6) != 3.0) return 63;
    if (round(-2.4) != -2.0 || round(-2.6) != -3.0) return 64;
    if (floor(1e300) != 1e300 || ceil(-1e300) != -1e300) return 65;
    if (fabs(-2.5) != 2.5) return 66;
    if (fabs(2.5) != 2.5) return 67;

    /* A remainder is the difference that leaves the divisor inside it, and the
       divisor's magnitude may be far from the dividend's, which a single
       division cannot express. */
    if (fmod(7.0, 3.0) != 1.0) return 68;
    if (fmod(-7.0, 3.0) != -1.0) return 69;
    if (fmod(7.0, -3.0) != 1.0) return 70;
    if (fmod(1.0, 7.0) != 1.0) return 71;
    if (fmod(6.0, 0.0) != 0.0) return 72;
    /* A quotient too large to count is reported as a remainder of zero, which
       is the only remainder a caller can act on without the bits the quotient
       cannot carry. */
    if (fmod(1e300, 1e-300) != 0.0) return 73;
    if (fmod(1e-300, 1e300) != 1e-300) return 74;

    if (fmin(2.0, 3.0) != 2.0 || fmax(2.0, 3.0) != 3.0) return 75;
    if (!near(hypot(3.0, 4.0), 5.0, 1e-15)) return 76;
    if (copysign(2.0, -1.0) != -2.0) return 77;
    if (copysign(-2.0, 1.0) != 2.0) return 78;

    /* The single-precision forms are the wide ones narrowed, so each is the
       wide answer within the range a narrow value can hold. */
    if (sqrtf(4.0f) != 2.0f) return 79;
    if (floorf(1.5f) != 1.0f) return 80;
    if (ceilf(1.5f) != 2.0f) return 81;
    if (fabsf(-1.5f) != 1.5f) return 82;
    if (fmodf(7.0f, 3.0f) != 1.0f) return 83;
    if (!near((double)powf(2.0f, 10.0f), 1024.0, 1e-6)) return 84;

    cc64_write(1, "S", 1);
    return 0;
}
