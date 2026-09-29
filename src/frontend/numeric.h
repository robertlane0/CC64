#ifndef CC64_NUMERIC_H
#define CC64_NUMERIC_H

#include <stdbool.h>
#include <stdint.h>

/* Project-owned decimal constant conversion.
 *
 * The compiler converts its own floating constants with these routines instead
 * of a host or target library routine, so a self-hosted build and the
 * bootstrap build execute the same code and therefore produce identical
 * objects. The conversion is exact-then-rounded: the significand is formed in
 * a 64-bit integer, powers of ten are applied exactly where they fit, the
 * remaining low-order information is carried as a sticky bit, and the final
 * binary64 value is rounded to nearest with ties to even. Inputs outside the
 * documented range are rejected rather than silently rounded, so an
 * unsupported constant is a diagnostic instead of a wrong value.
 *
 * Supported range: at most 19 significant decimal digits, a decimal exponent
 * between -27 and 19, and a result in the normal binary64 range. */

/* Returns the binary64 encoding of a decimal constant, or false when the
   constant is malformed or outside the supported range. */
bool cc64_decimal_to_double(const char *text, uint64_t *bits);

/* Narrows a binary64 value to binary32, rounding to nearest with ties to even
   and producing an infinity on overflow. */
uint32_t cc64_double_to_float(uint64_t bits);

/* Widens a binary32 encoding to the binary64 value that narrows back to it.
   The narrowing of a decimal constant has to be rounded by project code so a
   bootstrap build and a self-hosted build agree, but the widened value has to
   reach the encoder as a double. Widening is exact and is done here rather
   than by the host's float conversion, which is not available on the target
   and would not round the same way in both builds. */
uint64_t cc64_float_to_double(uint32_t bits);

#endif
