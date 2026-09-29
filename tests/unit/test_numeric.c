/* Project-owned decimal constant conversion tests. The reference values in
   this file were computed independently and are fixed here so that the test
   never consults a host or target library routine at run time. */
#include "frontend/numeric.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

static uint64_t convert(const char *text, bool *ok)
{
    uint64_t bits = 0UL;
    *ok = cc64_decimal_to_double(text, &bits);
    return bits;
}

static void expect_double(const char *text, uint64_t expected)
{
    bool ok = false;
    uint64_t bits = convert(text, &ok);
    if (!ok) {
        fprintf(stderr, "FAIL rejected %s\n", text);
        ++failures;
        return;
    }
    if (bits != expected) {
        fprintf(stderr, "FAIL %s: %016llx want %016llx\n", text,
                (unsigned long long)bits, (unsigned long long)expected);
        ++failures;
    }
}

static void expect_reject(const char *text)
{
    bool ok = false;
    (void)convert(text, &ok);
    if (ok) {
        fprintf(stderr, "FAIL accepted %s\n", text);
        ++failures;
    }
}

static void test_exact(void)
{
    expect_double("0.0", 0UL);
    expect_double("1.0", 0x3FF0000000000000UL);
    expect_double("2.0", 0x4000000000000000UL);
    expect_double("0.5", 0x3FE0000000000000UL);
    expect_double("-0.5", 0xBFE0000000000000UL);
    expect_double("1e5", 0x40F86A0000000000UL);
    expect_double("1e-5", 0x3EE4F8B588E368F1UL);
    expect_double("12345.6789", 0x40C81CD6E631F8A1UL);
}

static void test_rounding(void)
{
    /* Nearest with ties to even, verified against an independent computation. */
    expect_double("0.1", 0x3FB999999999999AUL);
    expect_double("0.2", 0x3FC999999999999AUL);
    expect_double("0.3", 0x3FD3333333333333UL);
    expect_double("3.14159265358979", 0x400921FB54442D11UL);
    expect_double("2.718281828459045", 0x4005BF0A8B145769UL);
    expect_double("0.30000000000000004", 0x3FD3333333333334UL);
    expect_double("1.0000000000000002", 0x3FF0000000000001UL);
    expect_double("9007199254740993", 0x4340000000000000UL);
    expect_double("0.1", 0x3FB999999999999AUL);
    /* A decimal exponent larger than any single power of ten that fits in a
       word is applied in steps, with the significand brought back to a word
       between them, so the whole range of the format is reachable. Each value
       below is the correctly rounded binary64, and the two at each end of the
       range are the smallest and the largest values the format can hold. */
    expect_double("1e19", 0x43E158E460913D00UL);
    expect_double("1e22", 0x4480F0CF064DD592UL);
    expect_double("1e23", 0x44B52D02C7E14AF6UL);
    expect_double("1e30", 0x46293E5939A08CEAUL);
    expect_double("1e100", 0x54B249AD2594C37DUL);
    expect_double("1e300", 0x7E37E43C8800759CUL);
    expect_double("1.7976931348623157e308", 0x7FEFFFFFFFFFFFFFUL);
    expect_double("6.02214076e23", 0x44DFE185CA57C517UL);
    expect_double("9.1093837015e-31", 0x39B279DCC8B6B7EDUL);
    expect_double("1.602176634e-19", 0x3C07A4DA290C1653UL);
    expect_double("1e-19", 0x3BFD83C94FB6D2ACUL);
    expect_double("1e-100", 0x2B2BFF2EE48E0530UL);
    expect_double("1e-300", 0x01A56E1FC2F8F359UL);
    expect_double("1e-320", 0x00000000000007E8UL);
    expect_double("4.9406564584124654e-324", 0x0000000000000001UL);
    expect_double("2.2250738585072014e-308", 0x0010000000000000UL);
    /* A constant below the smallest value the format holds rounds to zero,
       which is a value the format holds, so it is accepted rather than
       refused. A constant above the largest has no value to hold at all and is
       refused, because that is the case where the text is nearly always a
       mistake. */
    expect_double("1e-400", 0x0000000000000000UL);
    expect_double("123456789012345678901234567890.0", 0x45F8EE90FF6C373EUL);
}

static void test_range(void)
{
    /* A well-formed constant outside the format's range has no value to hold,
       and the malformed ones below have no digits to read at all. */
    expect_reject("1e400");
    expect_reject("1e99999999999");
    expect_reject("");
    expect_reject(".");
    expect_reject("abc");
    expect_reject("1.0.0");
    expect_reject("1e");
    expect_reject("0x1p3");
}

static void test_float_narrowing(void)
{
    struct NarrowCase {
        const char *text;
        uint32_t expected;
    } cases[] = {
        { "0.0f", 0x00000000U },
        { "1.0f", 0x3F800000U },
        { "0.5f", 0x3F000000U },
        { "3.14159265358979f", 0x40490FDBU },
        { "1e10f", 0x501502F9U },
        { "-2.5f", 0xC0200000U }
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char digits[32];
        size_t length = strlen(cases[i].text);
        if (length > 0U && (cases[i].text[length - 1U] == 'f' ||
                            cases[i].text[length - 1U] == 'F')) {
            --length;
        }
        if (length >= sizeof(digits)) {
            ++failures;
            continue;
        }
        memcpy(digits, cases[i].text, length);
        digits[length] = '\0';
        bool ok = false;
        uint64_t bits = convert(digits, &ok);
        if (!ok) {
            fprintf(stderr, "FAIL rejected %s\n", cases[i].text);
            ++failures;
            continue;
        }
        uint32_t narrowed = cc64_double_to_float(bits);
        if (narrowed != cases[i].expected) {
            fprintf(stderr, "FAIL %s: %08x want %08x\n", cases[i].text,
                    narrowed, cases[i].expected);
            ++failures;
        }
    }
    /* An overflow to infinity, an exact narrowing, and a flush to zero. */
    CHECK(cc64_double_to_float(0x47EFFFFFFFFFFFFFUL) == 0x7F800000U);
    CHECK(cc64_double_to_float(0x3810000000000000UL) == 0x00800000U);
    CHECK(cc64_double_to_float(0x3CA0000000000000UL) == 0x25000000U);
    CHECK(cc64_double_to_float(0x0000000000000001UL) == 0x00000000U);
}

/* The binary32 rounding is project code, and the encoder narrows again when
   it emits the constant, so widening the rounded encoding has to be exact.
   A widening that is not exact, or a constant whose four bytes were copied
   into an eight-byte value, makes every float constant a denormal. */
static void test_float_widening(void)
{
    struct RoundCase {
        uint32_t single;
        uint64_t wide;
    } cases[] = {
        { 0x00000000U, 0x0000000000000000UL },
        { 0x80000000U, 0x8000000000000000UL },
        { 0x3F800000U, 0x3FF0000000000000UL },
        { 0x40000000U, 0x4000000000000000UL },
        { 0xC0200000U, 0xC004000000000000UL },
        { 0x7F800000U, 0x7FF0000000000000UL },
        { 0xFF800000U, 0xFFF0000000000000UL },
        { 0x00000001U, 0x36A0000000000000UL },
        { 0x007FFFFFU, 0x380FFFFFC0000000UL },
        { 0x00800000U, 0x3810000000000000UL },
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t wide = cc64_float_to_double(cases[i].single);
        if (wide != cases[i].wide) {
            fprintf(stderr, "FAIL widen %08x: %016llx want %016llx\n",
                    cases[i].single, (unsigned long long)wide,
                    (unsigned long long)cases[i].wide);
            ++failures;
        }
    }
    /* Widening then narrowing is the identity on every binary32 value, and a
       NaN stays unordered rather than becoming a number. The subnormal range
       is the part where an inexact widening would show, because a subnormal
       has no implicit leading bit to shift into place. */
    for (uint32_t bits = 0U; bits < 0x7F800000U; bits += 9973U) {
        uint64_t wide = cc64_float_to_double(bits);
        if (cc64_double_to_float(wide) != bits) {
            fprintf(stderr, "FAIL round trip %08x\n", bits);
            ++failures;
            break;
        }
    }
    for (uint32_t bits = 0U; bits < 0x00800000U; bits += 997U) {
        uint64_t wide = cc64_float_to_double(bits);
        if (cc64_double_to_float(wide) != bits) {
            fprintf(stderr, "FAIL subnormal round trip %08x\n", bits);
            ++failures;
            break;
        }
    }
    CHECK((cc64_float_to_double(0x7FC00000U) & 0x7FFFFFFFFFFFFFFFUL) >
          0x7FEFFFFFFFFFFFFFUL);
}

static void test_signed_zero(void)
{
    bool ok = false;
    uint64_t bits = convert("-0.0", &ok);
    CHECK(ok);
    CHECK(bits == 0x8000000000000000UL);
}

int main(void)
{
    test_exact();
    test_rounding();
    test_range();
    test_float_narrowing();
    test_float_widening();
    test_signed_zero();
    if (failures == 0) {
        printf("numeric: decimal constant conversion groups passed\n");
        return 0;
    }
    fprintf(stderr, "numeric: %d failure(s)\n", failures);
    return 1;
}
