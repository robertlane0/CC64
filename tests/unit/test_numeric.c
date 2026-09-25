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
}

static void test_range(void)
{
    expect_reject("1e22");
    expect_reject("1.7976931348623157e308");
    expect_reject("1e-300");
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
    test_signed_zero();
    if (failures == 0) {
        printf("numeric: decimal constant conversion groups passed\n");
        return 0;
    }
    fprintf(stderr, "numeric: %d failure(s)\n", failures);
    return 1;
}
