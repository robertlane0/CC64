#ifndef CC64_TARGET_STDINT_H
#define CC64_TARGET_STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;
typedef signed long intmax_t;
typedef unsigned long uintmax_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255
#define INT16_MIN (-32768)
#define INT16_MAX 32767
#define UINT16_MAX 65535
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define INT64_MIN (-9223372036854775807L - 1L)
#define INT64_MAX 9223372036854775807L
#define UINT64_MAX 18446744073709551615UL
#define SIZE_MAX UINT64_MAX
#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

/* A constant of the least type that holds the value keeps its type through
   integer promotion on the target, where the suffix that does that is the one
   for the width of the least 32- and 64-bit types. */
#define INT8_C(value) value
#define INT16_C(value) value
#define INT32_C(value) value
#define INT64_C(value) value##L
#define UINT8_C(value) value
#define UINT16_C(value) value
#define UINT32_C(value) value##U
#define UINT64_C(value) value##UL
#define INTMAX_C(value) value##L
#define UINTMAX_C(value) value##UL

#endif
