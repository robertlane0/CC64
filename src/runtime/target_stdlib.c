/* Target numeric conversion and sorting for MS-DOS64.
 *
 * The integer conversions follow the C rules the compiler depends on: optional
 * sign, optional base prefix, optional sign, and a base that must match the
 * prefix. Overflow sets ERANGE and returns the saturated value, so the
 * front end's own overflow diagnostics keep working on the target. */

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CC64_LONG_MAX 0x7FFFFFFFFFFFFFFFUL
#define CC64_LONG_MIN 0x8000000000000000UL

static unsigned long convert_unsigned(const char *text, char **end, int base,
                                      bool *overflow)
{
    const char *cursor = text;
    unsigned long value = 0UL;
    *overflow = false;
    while (isspace((int)(unsigned char)*cursor)) ++cursor;
    if (*cursor == '+') ++cursor;
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
        base = 16;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }
    unsigned limit_base = (unsigned)base;
    for (;;) {
        int digit = -1;
        unsigned char byte = (unsigned char)*cursor;
        if (byte >= '0' && byte <= '9') digit = (int)(byte - '0');
        else if (byte >= 'a' && byte <= 'z') digit = (int)(byte - 'a') + 10;
        else if (byte >= 'A' && byte <= 'Z') digit = (int)(byte - 'A') + 10;
        if (digit < 0 || (unsigned)digit >= limit_base) break;
        if (value > (CC64_LONG_MAX - (unsigned long)digit) /
                        (unsigned long)limit_base) {
            *overflow = true;
        }
        value = value * (unsigned long)limit_base + (unsigned long)digit;
        ++cursor;
    }
    if (end != NULL) *end = (char *)cursor;
    return value;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    bool overflow = false;
    unsigned long value = convert_unsigned(text, end, base, &overflow);
    if (overflow) errno = ERANGE;
    return value;
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    return (unsigned long long)strtoul(text, end, base);
}

long strtol(const char *text, char **end, int base)
{
    const char *cursor = text;
    while (isspace((int)(unsigned char)*cursor)) ++cursor;
    bool negative = false;
    if (*cursor == '-' || *cursor == '+') {
        negative = *cursor == '-';
        ++cursor;
    }
    bool overflow = false;
    unsigned long magnitude = convert_unsigned(text, end, base, &overflow);
    if (overflow) {
        errno = ERANGE;
        return negative ? (long)CC64_LONG_MIN : (long)CC64_LONG_MAX;
    }
    if (negative) {
        if (magnitude > CC64_LONG_MAX + 1UL) {
            errno = ERANGE;
            return (long)CC64_LONG_MIN;
        }
        return (long)(CC64_LONG_MIN + magnitude);
    }
    if (magnitude > CC64_LONG_MAX) {
        errno = ERANGE;
        return (long)CC64_LONG_MAX;
    }
    return (long)magnitude;
}

long long strtoll(const char *text, char **end, int base)
{
    return (long long)strtol(text, end, base);
}

int atoi(const char *text)
{
    return (int)strtol(text, NULL, 10);
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

static unsigned long next_random = 1UL;

int rand(void)
{
    next_random = next_random * 1103515245UL + 12345UL;
    return (int)((next_random >> 16) & 0x7FFFUL);
}

void srand(unsigned int seed)
{
    next_random = (unsigned long)seed;
}

/* An insertion sort keeps the object writer's ordering stable and fully
   specified, which matters because the symbol order is part of the reproducible
   object bytes. */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    if (base == NULL || size == 0U || count < 2U) return;
    unsigned char *items = (unsigned char *)base;
    unsigned char *slot = (unsigned char *)malloc(size);
    if (slot == NULL) return;
    size_t index = 1U;
    while (index < count) {
        memcpy(slot, items + index * size, size);
        size_t position = index;
        while (position > 0U &&
               compare(items + (position - 1U) * size, slot) > 0) {
            memcpy(items + position * size, items + (position - 1U) * size, size);
            --position;
        }
        memcpy(items + position * size, slot, size);
        ++index;
    }
    free(slot);
}
