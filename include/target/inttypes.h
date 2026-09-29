#ifndef CC64_TARGET_INTTYPES_H
#define CC64_TARGET_INTTYPES_H

#include <stdint.h>

/* The target's integer types carry the same width as the target's own, and the
   format macros are the portable spellings a program uses to print one, so a
   caller that includes this header gets the same text it wrote. */

typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;

intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);
intmax_t strtoimax(const char *text, char **end, int base);
uintmax_t strtoumax(const char *text, char **end, int base);

#endif
