#include "frontend/numeric.h"

#include <stddef.h>

/* Decimal constant conversion for the compiler front end.
 *
 * The value is built as an exact rational n * 2^-scale with a sticky bit for
 * everything that was discarded, then rounded once to the nearest binary64
 * with ties to even. Because the whole conversion is project code, the
 * bootstrap build and a self-hosted build agree bit for bit without depending
 * on any library's conversion routine. */

#define CC64_DECIMAL_MAX_DIGITS 19U
/* Ten to the largest power still representable in 64 bits. */
#define CC64_DECIMAL_MAX_SCALE_EXPONENT 19
/* Five to the largest power still representable in 64 bits. */
#define CC64_DECIMAL_MAX_DIVISOR_EXPONENT 27

static const uint64_t power_of_ten[CC64_DECIMAL_MAX_SCALE_EXPONENT + 1] = {
    1UL, 10UL, 100UL, 1000UL, 10000UL, 100000UL, 1000000UL, 10000000UL,
    100000000UL, 1000000000UL, 10000000000UL, 100000000000UL,
    1000000000000UL, 10000000000000UL, 100000000000000UL,
    1000000000000000UL, 10000000000000000UL, 100000000000000000UL,
    1000000000000000000UL, 10000000000000000000UL
};

/* 5^0 .. 5^27, the odd factor removed with the binary scale when dividing by
   a power of ten. */
static const uint64_t power_of_five[CC64_DECIMAL_MAX_DIVISOR_EXPONENT + 1] = {
    1UL, 5UL, 25UL, 125UL, 625UL, 3125UL, 15625UL, 78125UL, 390625UL,
    1953125UL, 9765625UL, 48828125UL, 244140625UL, 1220703125UL,
    6103515625UL, 30517578125UL, 152587890625UL, 762939453125UL,
    3814697265625UL, 19073486328125UL, 95367431640625UL,
    476837158203125UL, 2384185791015625UL, 11920928955078125UL,
    59604644775390625UL, 298023223876953125UL, 1490116119384765625UL,
    7450580596923828125UL
};

static unsigned bit_length(uint64_t value)
{
    unsigned length = 0U;
    while (value != 0U) {
        ++length;
        value >>= 1;
    }
    return length;
}

/* A 128-bit intermediate is required to divide a full-width significand by a
   power of five without losing the low bits that decide correct rounding. */
typedef struct Wide {
    uint64_t high;
    uint64_t low;
} Wide;

static void wide_from_u64(uint64_t value, Wide *result)
{
    result->high = 0UL;
    result->low = value;
}

static void wide_shift_left(const Wide *value, unsigned bits, Wide *result)
{
    if (bits == 0U) {
        result->high = value->high;
        result->low = value->low;
        return;
    }
    if (bits >= 128U) {
        result->high = 0UL;
        result->low = 0UL;
        return;
    }
    if (bits >= 64U) {
        result->high = value->low << (bits - 64U);
        result->low = 0UL;
        return;
    }
    result->high = (value->high << bits) | (value->low >> (64U - bits));
    result->low = value->low << bits;
}

static uint64_t wide_bit(const Wide *value, unsigned bit)
{
    uint64_t word = bit >= 64U ? value->high : value->low;
    return (word >> (bit & 63U)) & 1UL;
}

/* Restoring binary division of a 128-bit value by a 64-bit divisor. The
   running remainder stays below the divisor, so the 65-bit intermediate is
   carried by the shift's carry flag. */
static void wide_divide(const Wide *value, uint64_t divisor, Wide *quotient,
                        uint64_t *remainder)
{
    Wide result;
    result.high = 0UL;
    result.low = 0UL;
    uint64_t rest = 0UL;
    int bit;
    for (bit = 127; bit >= 0; --bit) {
        uint64_t source = wide_bit(value, (unsigned)bit);
        bool carry = (rest >> 63) != 0UL;
        rest = (rest << 1) | source;
        bool take = carry || rest >= divisor;
        if (take) rest -= divisor;
        if (bit < 64) {
            if (take) result.low |= 1UL << bit;
        } else {
            if (take) result.high |= 1UL << (bit - 64);
        }
    }
    quotient->high = result.high;
    quotient->low = result.low;
    *remainder = rest;
}

/* Full 64 by 64 product. The result always fits in 128 bits. */
static void wide_multiply(uint64_t left, uint64_t right, Wide *product)
{
    uint64_t left_low = left & 0xFFFFFFFFUL;
    uint64_t left_high = left >> 32;
    uint64_t right_low = right & 0xFFFFFFFFUL;
    uint64_t right_high = right >> 32;
    uint64_t p0 = left_low * right_low;
    uint64_t p1 = left_low * right_high;
    uint64_t p2 = left_high * right_low;
    uint64_t p3 = left_high * right_high;
    uint64_t carry = p0 >> 32;
    uint64_t sum;
    product->low = p0 & 0xFFFFFFFFUL;
    sum = (p1 & 0xFFFFFFFFUL) + (p2 & 0xFFFFFFFFUL) + carry;
    product->low |= (sum & 0xFFFFFFFFUL) << 32;
    carry = (p1 >> 32) + (p2 >> 32) + (sum >> 32);
    sum = (p3 & 0xFFFFFFFFUL) + carry;
    product->high = (sum & 0xFFFFFFFFUL) | ((p3 >> 32) + (sum >> 32)) << 32;
}

/* Keeps the top 64 bits of a 128-bit intermediate. Every bit that falls below
   the retained window is reported through truncated, which the final rounding
   uses as a sticky input, and through dropped, the amount by which the binary
   scale must grow to describe the retained value. */
static uint64_t wide_reduce(const Wide *value, bool *truncated, unsigned *dropped)
{
    *dropped = 0U;
    if (value->high == 0UL) return value->low;
    unsigned amount = bit_length(value->high);
    uint64_t low;
    if (amount < 64U) {
        if ((value->low & ((1UL << (amount - 1U)) - 1UL)) != 0UL) *truncated = true;
        low = (value->high << (64U - amount)) | (value->low >> amount);
    } else {
        if ((value->high & 0x7FFFFFFFFFFFFFFFUL) != 0UL ||
            value->low != 0UL) {
            *truncated = true;
        }
        low = value->high;
    }
    *dropped = amount;
    return low;
}

/* Rounds the exact value numerator * 2^-scale (with a nonzero remainder when
   sticky is set) to a binary64 encoding, nearest with ties to even. The result
   is first expressed as an integer multiple of 2^-fraction_bits, which is the
   53-bit quantum for a normal result and the least subnormal quantum
   otherwise, and is then packed. */
static bool assemble_double(uint64_t numerator, int64_t scale, bool sticky,
                            bool negative, uint64_t *bits)
{
    if (numerator == 0U) {
        *bits = negative ? (1UL << 63) : 0UL;
        return true;
    }
    unsigned length = bit_length(numerator);
    int64_t exponent = (int64_t)length - 1 - (int64_t)scale;
    int64_t fraction_bits = exponent >= -1022 ? 52 - exponent : 1074;
    int64_t shift = fraction_bits - (int64_t)scale;
    uint64_t significand;
    if (shift >= 0) {
        if (shift >= 64) return false;
        significand = numerator << (unsigned)shift;
        if (significand >> (unsigned)shift != numerator) return false;
    } else {
        unsigned right = (unsigned)(-shift);
        bool guard = false;
        bool rest = sticky;
        if (right > 64U) {
            significand = 0UL;
        } else if (right == 64U) {
            significand = 0UL;
            guard = ((numerator >> 63U) & 1UL) != 0UL;
            rest = rest || (numerator & 0x7FFFFFFFFFFFFFFFUL) != 0UL;
        } else {
            significand = numerator >> right;
            guard = ((numerator >> (right - 1U)) & 1UL) != 0UL;
            if (right > 1U && (numerator & ((1UL << (right - 1U)) - 1UL)) != 0UL) {
                rest = true;
            }
        }
        if (guard && (rest || (significand & 1UL) != 0UL)) significand += 1UL;
    }
    if (significand == 0UL) {
        *bits = negative ? (1UL << 63) : 0UL;
        return true;
    }
    unsigned width = bit_length(significand);
    int64_t unbiased = (int64_t)width - 1 - fraction_bits;
    if (unbiased < -1022) {
        *bits = (negative ? (1UL << 63) : 0UL) | significand;
        return true;
    }
    if (unbiased > 1023) return false;
    if (width > 53U) significand >>= (width - 53U);
    *bits = ((uint64_t)(unbiased + 1023) << 52) | (significand & 0x000FFFFFFFFFFFFFUL);
    if (negative) *bits |= 1UL << 63;
    return true;
}

bool cc64_decimal_to_double(const char *text, uint64_t *bits)
{
    if (text == NULL || bits == NULL) return false;
    size_t index = 0U;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
    }
    uint64_t mantissa = 0UL;
    unsigned digits = 0U;
    int64_t exponent = 0;
    bool truncated = false;
    bool seen_digit = false;
    for (;;) {
        unsigned char byte = (unsigned char)text[index];
        if (byte < '0' || byte > '9') break;
        seen_digit = true;
        if (digits < CC64_DECIMAL_MAX_DIGITS) {
            mantissa = mantissa * 10UL + (uint64_t)(byte - '0');
            ++digits;
        } else {
            /* Extra digits are dropped but still shift the decimal point. */
            ++exponent;
            if (mantissa != 0UL) truncated = true;
        }
        ++index;
    }
    if (text[index] == '.') {
        ++index;
        for (;;) {
            unsigned char byte = (unsigned char)text[index];
            if (byte < '0' || byte > '9') break;
            seen_digit = true;
            if (digits < CC64_DECIMAL_MAX_DIGITS) {
                mantissa = mantissa * 10UL + (uint64_t)(byte - '0');
                ++digits;
                --exponent;
            } else {
                truncated = true;
            }
            ++index;
        }
    }
    if (!seen_digit) return false;
    if (text[index] == 'e' || text[index] == 'E') {
        ++index;
        bool negative_exponent = false;
        if (text[index] == '+' || text[index] == '-') {
            negative_exponent = text[index] == '-';
            ++index;
        }
        if (text[index] < '0' || text[index] > '9') return false;
        unsigned value = 0U;
        while (text[index] >= '0' && text[index] <= '9') {
            if (value < 10000U) value = value * 10U + (unsigned)(text[index] - '0');
            ++index;
        }
        if (value > (unsigned)CC64_DECIMAL_MAX_DIVISOR_EXPONENT * 4U) return false;
        exponent += negative_exponent ? -(int64_t)value : (int64_t)value;
    }
    if (text[index] != '\0') return false;
    if (mantissa == 0UL) {
        *bits = negative ? (1UL << 63) : 0UL;
        return true;
    }
    /* Nineteen digits always fit in 64 bits, and every product below runs in
       128 bits, so the significand is never trimmed before conversion. */
    uint64_t numerator = mantissa;
    int64_t scale = 0;
    unsigned dropped = 0U;
    if (exponent > 0) {
        if (exponent > CC64_DECIMAL_MAX_SCALE_EXPONENT) return false;
        Wide product;
        wide_multiply(mantissa, power_of_ten[exponent], &product);
        numerator = wide_reduce(&product, &truncated, &dropped);
        /* Dropping low bits of the product scales the retained value up. */
        scale = -(int64_t)dropped;
    } else if (exponent < 0) {
        int64_t divisor_exponent = -exponent;
        if (divisor_exponent > CC64_DECIMAL_MAX_DIVISOR_EXPONENT) return false;
        /* 10^k is 2^k times 5^k, so the value is (m / 5^k) scaled by 2^-k. The
           significand is first widened by a power of two far enough that the
           quotient still carries at least 64 bits; the division itself runs in
           128 bits so that rounding sees every bit that decides it. */
        uint64_t divisor = power_of_five[divisor_exponent];
        unsigned width = bit_length(mantissa);
        unsigned divisor_width = bit_length(divisor);
        if (divisor_width > 64U) return false;
        unsigned widen = 63U + divisor_width - width;
        if (width + widen > 128U) return false;
        Wide source;
        Wide scaled;
        wide_from_u64(mantissa, &source);
        wide_shift_left(&source, widen, &scaled);
        Wide quotient;
        uint64_t remainder = 0UL;
        wide_divide(&scaled, divisor, &quotient, &remainder);
        numerator = wide_reduce(&quotient, &truncated, &dropped);
        if (remainder != 0UL) truncated = true;
        scale = (int64_t)widen + (int64_t)dropped + divisor_exponent;
    }
    return assemble_double(numerator, scale, truncated, negative, bits);
}

uint32_t cc64_double_to_float(uint64_t bits)
{
    uint64_t exponent_field = (bits >> 52) & 0x7FFUL;
    uint64_t mantissa = bits & 0x000FFFFFFFFFFFFFUL;
    bool negative = (bits >> 63) != 0UL;
    if (exponent_field == 0x7FFUL) {
        uint32_t pattern = 0x7F800000U | (mantissa != 0U ? 0x00400000U : 0U);
        return negative ? pattern | 0x80000000U : pattern;
    }
    if (exponent_field == 0UL && mantissa == 0UL) {
        return negative ? 0x80000000U : 0U;
    }
    int64_t exponent = (int64_t)exponent_field - 1023;
    uint64_t significand = mantissa | (1UL << 52);
    /* binary64 keeps 53 bits, binary32 keeps 24.
       exponent_field - 1023 + 52 - 23 */
    int64_t shift = 52 - 23;
    if (exponent_field == 0UL) shift += 1;
    uint64_t result;
    if (shift <= 0) {
        result = significand << (unsigned)(-shift);
    } else {
        unsigned amount = (unsigned)shift;
        bool round_bit = ((significand >> (amount - 1U)) & 1UL) != 0UL;
        bool sticky = false;
        if (amount > 1U) {
            sticky = (significand & ((1UL << (amount - 1U)) - 1UL)) != 0UL;
        }
        result = significand >> amount;
        if (round_bit && (sticky || (result & 1UL) != 0UL)) result += 1UL;
        if (result >> 24U) {
            result >>= 1;
            ++exponent;
        }
    }
    if (exponent > 127) return negative ? 0xFF800000U : 0x7F800000U;
    if (exponent < -126) {
        if (exponent < -149) return negative ? 0x80000000U : 0U;
        uint32_t subnormal = (uint32_t)(result >> (unsigned)(-126 - exponent));
        return negative ? subnormal | 0x80000000U : subnormal;
    }
    uint32_t value = ((uint32_t)(exponent + 127) << 23) | ((uint32_t)result & 0x007FFFFFU);
    return negative ? value | 0x80000000U : value;
}
