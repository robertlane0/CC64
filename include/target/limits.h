#ifndef CC64_TARGET_LIMITS_H
#define CC64_TARGET_LIMITS_H

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#define LONG_MIN (-9223372036854775807L - 1L)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL

/* The target names a file with a drive letter, a separator, and an eight-byte
   stem with a three-byte extension, so the longest name a caller should build
   has room for all of that and a terminator. */
#define PATH_MAX 260
#define NAME_MAX 255
#define FILENAME_MAX PATH_MAX
#define HOST_NAME_MAX 64

#endif
