#!/usr/bin/env python3
"""The target conformance corpus.

One definition of the cases, so the host-built compiler and a target-built
compiler are held to the same programs, the same exit codes, and the same
expected output. A case is a name, the source text, the image format, the exit
code the program must return, the text the program must print, and an optional
command tail.
"""

from __future__ import annotations

LIBRARY_PROGRAM = (
    "#include <errno.h>\n"
    "#include <stdarg.h>\n"
    "#include <stdint.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "int cc64_putc(int);\n"
    "static char block[65552];\n"
    # Eight unnamed arguments: the seventh and eighth arrive on the stack, so
    # this fails unless the variadic walk reaches past the argument registers
    # (D-053).
    "static int wide(int count, ...) {\n"
    "  va_list args;\n"
    "  int total;\n"
    "  va_start(args, count);\n"
    "  total = va_arg(args, int) + va_arg(args, int) + va_arg(args, int)\n"
    "        + va_arg(args, int) + va_arg(args, int) + va_arg(args, int)\n"
    "        + va_arg(args, int);\n"
    "  va_end(args);\n"
    "  return total;\n"
    "}\n"
    "int main(void) {\n"
    "  cc64_write(1, \"A\", 1);\n"
    "  cc64_putc(48 + (int)strlen(\"abcd\"));\n"
    "  cc64_write(1, \"B\", 1);\n"
    "  char *copy = (char *)malloc(16);\n"
    "  if (copy == 0) return 1;\n"
    "  copy[0] = 'Z'; copy[1] = 0;\n"
    "  cc64_write(1, copy, 1);\n"
    "  if (strcmp(copy, \"Z\") != 0) return 2;\n"
    "  if (strlen(\"xy\") != 2) return 3;\n"
    "  memset(copy, 'W', 4);\n"
    "  if (copy[0] != 'W') return 4;\n"
    "  cc64_write(1, \"C\", 1);\n"
    "  if (fputs(\"D\", stdout) != 0) return 5;\n"
    "  char text[32];\n"
    "  int n = snprintf(text, sizeof text, \"%s=%u:%d\", \"k\", 42u, -7);\n"
    "  if (n != 7) return 6;\n"
    "  if (strcmp(text, \"k=42:-7\") != 0) return 7;\n"
    "  cc64_write(1, text, (unsigned long)n);\n"
    "  if (wide(7, 1, 2, 3, 4, 5, 6, 7) != 28) return 8;\n"
    # A refused allocation has to read as a null pointer, and a refused open has
    # to read as a null stream (D-052).
    "  if (malloc(0x40000000UL) != 0) return 9;\n"
    "  if (fopen(\"NOSUCH.FILE\", \"rb\") != 0) return 10;\n"
    # The target keeps thirteen handles for a process, so a closed read handle
    # has to be released rather than leaked (D-055).
    "  int i;\n"
    "  for (i = 0; i < 20; ++i) {\n"
    "    FILE *f = fopen(\"HELLO.TXT\", \"rb\");\n"
    "    if (f == 0) return 11;\n"
    "    if (fclose(f) != 0) return 12;\n"
    "  }\n"
    "  FILE *made = fopen(\"C64L.TMP\", \"wb\");\n"
    "  if (made == 0) return 13;\n"
    "  if (fwrite(\"x\", 1, 1, made) != 1) return 14;\n"
    "  if (fclose(made) != 0) return 15;\n"
    # A transfer past the sixteen-bit service count: the target's read and
    # write services carry the byte count in a sixteen-bit field, so stdio has
    # to satisfy a request that large in steps (D-057). 65551 % 26 is 5.
    "  size_t index;\n"
    "  for (index = 0; index < sizeof block; ++index)\n"
    "    block[index] = (char)('a' + (int)(index % 26));\n"
    "  FILE *bulk = fopen(\"C64L.BIN\", \"wb\");\n"
    "  if (bulk == 0) return 16;\n"
    "  if (fwrite(block, 1, sizeof block, bulk) != sizeof block) return 17;\n"
    "  if (fclose(bulk) != 0) return 18;\n"
    "  for (index = 0; index < sizeof block; ++index) block[index] = 0;\n"
    "  FILE *source = fopen(\"C64L.BIN\", \"rb\");\n"
    "  if (source == 0) return 19;\n"
    "  if (fread(block, 1, sizeof block, source) != sizeof block) return 20;\n"
    "  if (fclose(source) != 0) return 21;\n"
    "  if (block[0] != 'a' || block[sizeof block - 1] != 'f') return 22;\n"
    # An unsigned conversion has to reach the whole unsigned range, and the
    # most negative signed value has to parse (D-058).
    "  errno = 0;\n"
    "  if (strtoul(\"18446744073709551615\", 0, 10) != 18446744073709551615UL)\n"
    "    return 23;\n"
    "  if (errno != 0) return 24;\n"
    "  if (SIZE_MAX != 18446744073709551615UL) return 25;\n"
    "  if (strtol(\"-9223372036854775808\", 0, 10) >= 0L) return 26;\n"
    "  if (errno != 0) return 27;\n"
    "  free(copy);\n"
    "  cc64_write(1, \"E\", 1);\n"
    "  return 9;\n"
    "}\n"
)


def compile_and_link(work: pathlib.Path, source_text: str, name: str,
                     image_format: str = "raw") -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    image = work / (f"{name}.mz" if image_format == "mz64" else f"{name}.com")
    source.write_text(source_text, encoding="utf-8")
    run([str(ROOT / "cc64"), "-I", str(ROOT / "include/target"),
         "-I", str(ROOT / "include/cc64"), "-c", str(source), "-o", str(obj)], ROOT)
    command = [str(ROOT / "cc64"), "--link"]
    if image_format == "mz64":
        command += ["--format", "mz64"]
    command += [str(obj), "-o", str(image)]
    run(command, ROOT)
    return image


FILE_WRITE_PROGRAM = (
    "int cc64_create(const char *, int);\n"
    "int cc64_open(const char *);\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "int cc64_read(int, void *, unsigned long);\n"
    "int cc64_close(int);\n"
    "char scratch[8];\n"
    "int main(void) {\n"
    "  int handle = cc64_create(\"CC64W.TXT\", 0);\n"
    "  if (handle < 0) return 1;\n"
    "  if (cc64_write(handle, \"hello\", 5) != 5) return 2;\n"
    "  if (cc64_close(handle) != 0) return 3;\n"
    "  handle = cc64_open(\"CC64W.TXT\");\n"
    "  if (handle < 0) return 4;\n"
    "  if (cc64_read(handle, scratch, 5) != 5) return 5;\n"
    "  cc64_close(handle);\n"
    "  if (scratch[0] != 'h') return 6;\n"
    "  if (scratch[4] != 'o') return 7;\n"
    "  return 7;\n"
    "}\n"
)

SEEK_PROGRAM = (
    "int cc64_open(const char *);\n"
    "int cc64_close(int);\n"
    "int cc64_lseek(int, long, int);\n"
    "int cc64_read(int, void *, unsigned long);\n"
    "char scratch[8];\n"
    "int main(void) {\n"
    # A refused service reports -1, not the target's small positive code, so a
    # failure can never be read back as a handle or an offset (CC64 D-052).
    "  if (cc64_open(\"NOSUCH.FILE\") != -1) return 1;\n"
    "  int handle = cc64_open(\"HELLO.TXT\");\n"
    "  if (handle < 0) return 2;\n"
    "  if (cc64_lseek(999, 0L, 0) != -1L) return 3;\n"
    "  if (cc64_lseek(handle, 0L, 3) != -1L) return 4;\n"
    "  long size = cc64_lseek(handle, 0L, 2);\n"
    "  if (size <= 0L) return 5;\n"
    "  if (cc64_lseek(handle, 0L, 0) != 0L) return 6;\n"
    "  if (cc64_lseek(handle, 1L, 0) != 1L) return 7;\n"
    "  if (cc64_lseek(handle, 0L, 1) != 1L) return 8;\n"
    "  if (cc64_lseek(handle, 0L, 0) != 0L) return 9;\n"
    "  if (cc64_lseek(handle, 100000L, 0) != -1L) return 10;\n"
    "  if (cc64_lseek(handle, 0L, 0) != 0L) return 11;\n"
    "  if (cc64_read(handle, scratch, 1) != 1) return 12;\n"
    "  if (cc64_read(999, scratch, 1) != -1) return 13;\n"
    "  cc64_close(handle);\n"
    "  return 7;\n"
    "}\n"
)


VARARG_PROGRAM = (
    "#include <stdarg.h>\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "static int sum(int count, ...) {\n"
    "  va_list args;\n"
    "  va_start(args, count);\n"
    "  int total = 0;\n"
    "  int index;\n"
    "  for (index = 0; index < count; ++index) total += va_arg(args, int);\n"
    "  va_end(args);\n"
    "  return total;\n"
    "}\n"
    "static int named(int first, const char *second, int third, ...) {\n"
    "  va_list args;\n"
    "  va_start(args, third);\n"
    "  int extra = va_arg(args, int);\n"
    "  long more = va_arg(args, long);\n"
    "  va_end(args);\n"
    "  return first + (int)second[0] + third + extra + (int)more;\n"
    "}\n"
    "int main(void) {\n"
    "  int total = sum(4, 1, 2, 3, 4);\n"
    "  int mixed = named(1, \"A\", 2, 30, 40L);\n"
    "  cc64_write(1, \"s=\", 2);\n"
    "  if (total == 10) cc64_write(1, \"10\", 2); else cc64_write(1, \"??\", 2);\n"
    "  cc64_write(1, \" n=\", 3);\n"
    "  if (mixed == 138) cc64_write(1, \"138\", 3); else cc64_write(1, \"???\", 3);\n"
    "  cc64_write(1, \"\\n\", 1);\n"
    "  if (total != 10) return 1;\n"
    "  if (mixed != 138) return 2;\n"
    "  return 9;\n"
    "}\n"
)


# The command tail is everything the shell was given after the program name,
# so "C64H AA BB" starts the program with two arguments. This case is the
# observable check of the startup contract: tail copy, in-place split, and the
# null terminator after the last entry.
# Relational operators are the only part of the comparison lowering that the
# host bootstrap cannot check, because the bootstrap host compiler generates the
# comparisons itself. This case pins every signed and unsigned relation, plus
# the loop shape that an unsigned ">" drives, so a wrong condition code shows up
# as a wrong exit code rather than as a silent hang.
# The conditional operator converts both operands, so a choice between two
# string literals, between a literal and a pointer, and between an array and a
# pointer all have to work at run time and not only survive the front end.
CONDITIONAL_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static const char *pick(int flag)\n"
    "{\n"
    "    return flag ? \"yes\" : \"no\";\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "    const char *a = 1 ? \"alpha\" : \"beta\";\n"
    "    char local[4] = \"xy\";\n"
    "    const char *b = 0 ? local : \"gamma\";\n"
    "    cc64_write(1, a, 2);\n"
    "    cc64_write(1, b, 2);\n"
    "    cc64_write(1, pick(1), 3);\n"
    "    cc64_write(1, \"\\n\", 1);\n"
    "    if (a[0] != 'a' || b[0] != 'g') return 1;\n"
    "    if (pick(0)[1] != 'o') return 2;\n"
    "    return 9;\n"
    "}\n"
)

# Aggregate initializers are written by the initializer writer rather than by
# expression code, so a case that only compiles would not notice a dropped
# element. This one checks the values at run time: a partially initialized
# array, a nested brace initializer, a struct with an array member, a union, a
# string initializer of an exact and a short length, and a designated-free
# trailing element.
INITIALIZER_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "struct Inner { int a; int b; };\n"
    "struct Outer { struct Inner inner; char name[4]; long tail; };\n"
    "union Mix { long whole; int halves[2]; };\n"
    "static int short_array[5] = {1, 2};\n"
    "static int nested[2][3] = {{1, 2, 3}, {4, 5, 6}};\n"
    "static struct Inner inner_value = {7, 8};\n"
    "static struct Outer outer_value = {{1, 2}, \"ab\", 9};\n"
    "static union Mix mix_value;\n"
    "static char exact[3] = \"xyz\";\n"
    "static char shorter[6] = \"hi\";\n"
    "int main(void)\n"
    "{\n"
    "    int local[3] = {4, 5, 6};\n"
    "    struct Inner local_inner = {3, 4};\n"
    "    mix_value.whole = 0x000100020001L;\n"
    "    if (short_array[0] != 1 || short_array[1] != 2) return 1;\n"
    "    if (short_array[2] != 0 || short_array[4] != 0) return 2;\n"
    "    if (nested[0][2] != 3 || nested[1][0] != 4 || nested[1][2] != 6) return 3;\n"
    "    if (inner_value.a != 7 || inner_value.b != 8) return 4;\n"
    "    if (outer_value.inner.a != 1 || outer_value.inner.b != 2) return 5;\n"
    "    if (outer_value.name[0] != 'a' || outer_value.name[1] != 'b') return 6;\n"
    "    if (outer_value.name[2] != 0) return 7;\n"
    "    if (outer_value.tail != 9) return 8;\n"
    "    if (exact[2] != 'z' || exact[0] != 'x') return 9;\n"
    "    if (shorter[1] != 'i' || shorter[2] != 0 || shorter[5] != 0) return 10;\n"
    "    if (local[2] != 6 || local_inner.b != 4) return 11;\n"
    "    if (mix_value.halves[0] != 0x00020001) return 12;\n"
    "    if (mix_value.halves[1] != 0x0001) return 13;\n"
    "    cc64_write(1, \"I\", 1);\n"
    "    return 9;\n"
    "}\n"
)

COMPARE_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static void count_down(unsigned long value, unsigned long base)\n"
    "{\n"
    "    char digits[72];\n"
    "    unsigned long length = 0UL;\n"
    "    while (value > 0UL) {\n"
    "        digits[length] = (char)(48 + (int)(value % base));\n"
    "        ++length;\n"
    "        value = value / base;\n"
    "    }\n"
    "    while (length > 0UL) { --length; cc64_write(1, digits + length, 1); }\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "    count_down(42UL, 10UL);\n"
    "    cc64_write(1, \"\\n\", 1);\n"
    "    if (3UL > 4UL) return 1;\n"
    "    if (4UL <= 3UL) return 2;\n"
    "    if (!(4UL >= 4UL)) return 3;\n"
    "    if (3UL >= 4UL) return 4;\n"
    "    if (-1 > 0) return 5;\n"
    "    if (2 >= 3) return 6;\n"
    "    if (2 <= 1) return 7;\n"
    "    if (-5 > -9) cc64_write(1, \"K\", 1);\n"
    "    if (-9 < -5) cc64_write(1, \"=\", 1);\n"
    "    return 9;\n"
    "}\n"
)

# The target library's own interface: an extension the header declares, the
# `v` form of each printf function, and the count a stream sink returns. The
# count is the part a case that only checked the printed text would miss: the
# text goes out through the same sink either way, so only the return value
# shows that a stream write was counted.
LIBRARY_VFORM_PROGRAM = (
    "#include <stdarg.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "static int emit(FILE *stream, const char *format, ...) {\n"
    "  va_list arguments;\n"
    "  va_start(arguments, format);\n"
    "  int written = vfprintf(stream, format, arguments);\n"
    "  va_end(arguments);\n"
    "  return written;\n"
    "}\n"
    # A variadic walk reads one whole eight-byte slot per argument, so a
    # 64-bit conversion has to read the whole slot and a 32-bit one only its
    # low half. Reading an int for `%ld` printed the wrong number for any
    # argument that does not fit in 32 bits.
    "static int wrap(char *text, unsigned long capacity, const char *format, ...) {\n"
    "  va_list arguments;\n"
    "  va_start(arguments, format);\n"
    "  int written = vsnprintf(text, capacity, format, arguments);\n"
    "  va_end(arguments);\n"
    "  return written;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "  char *copy = strdup(\"hello\");\n"
    "  if (copy == 0) return 1;\n"
    "  if (strcmp(copy, \"hello\") != 0) return 2;\n"
    "  free(copy);\n"
    # A stream sink counts the characters it wrote, so the printf family
    # returns a real count rather than zero.
    "  if (emit(stdout, \"%s-%d\", \"v\", 42) != 4) return 3;\n"
    "  if (emit(stdout, \"\\n\") != 1) return 4;\n"
    "  char text[32];\n"
    "  if (wrap(text, sizeof text, \"%ld\", 7L) != 1) return 5;\n"
    "  if (strcmp(text, \"7\") != 0) return 6;\n"
    "  if (wrap(text, sizeof text, \"%lu\", 99UL) != 2) return 7;\n"
    "  if (wrap(text, sizeof text, \"%lld\", 1234567890123LL) != 13) return 8;\n"
    "  if (strcmp(text, \"1234567890123\") != 0) return 9;\n"
    "  if (wrap(text, sizeof text, \"%x\", 255u) != 2) return 10;\n"
    "  if (strcmp(text, \"ff\") != 0) return 11;\n"
    "  if (wrap(text, sizeof text, \"[%5d]\", 42) != 7) return 12;\n"
    "  if (strcmp(text, \"[   42]\") != 0) return 13;\n"
    "  return 9;\n"
    "}\n"
)

# Floating arithmetic and comparison. The `f` suffix rounds to binary32 and
# the plain spelling is binary64, so a constant's rounding is checked by
# comparing against a value of the other width: a binary32 constant widened
# into a binary64 constant is the same number, and a binary32 constant that
# lost its high half is not. Every relation is checked in both directions,
# because a comparison that inverts still agrees with itself on equality.
# A zero-filled object is only observed to be zero if it is read before it is
# written. A case that assigns first cannot tell a zero-initialised BSS from an
# uninitialised one, and the loader's zeroing is what this checks.
BSS_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "int zeroed;\n"
    "int zeroed_tail[8];\n"
    "char zeroed_text[16];\n"
    "static long zeroed_static;\n"
    "int initialised = 7;\n"
    "int main(void)\n"
    "{\n"
    "    if (zeroed != 0) return 1;\n"
    "    if (zeroed_tail[0] != 0 || zeroed_tail[7] != 0) return 2;\n"
    "    if (zeroed_text[0] != 0 || zeroed_text[15] != 0) return 3;\n"
    "    if (zeroed_static != 0L) return 4;\n"
    # An initialised neighbour shows the section is present rather than the
    # whole image having been left blank.
    "    if (initialised != 7) return 5;\n"
    "    long i;\n"
    "    for (i = 0L; i < 8L; ++i) if (zeroed_tail[i] != 0) return 6;\n"
    "    for (i = 0L; i < 16L; ++i) if (zeroed_text[i] != 0) return 7;\n"
    "    zeroed = 9;\n"
    "    if (zeroed != 9) return 8;\n"
    "    cc64_write(1, \"B\", 1);\n"
    "    return 9;\n"
    "}\n"
)

# Recursion exercises the frame and call model at more than one depth: a
# leaf, a self call, a mutual pair, and the deepest call the target's stack
# allows. A frame that is not restored, or an argument that is not placed in
# its own slot, shows up as a wrong answer at depth rather than at the first
# call.
RECURSION_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }\n"
    "static int is_even(int n);\n"
    "static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }\n"
    "static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }\n"
    "static int depth_sum(int n) { return n == 0 ? 0 : n + depth_sum(n - 1); }\n"
    "int main(void)\n"
    "{\n"
    "    if (fact(10) != 3628800) return 1;\n"
    "    if (fact(0) != 1) return 2;\n"
    "    if (is_even(100) != 1) return 3;\n"
    "    if (is_odd(101) != 1) return 4;\n"
    "    if (is_even(99) != 0) return 5;\n"
    "    if (depth_sum(100) != 5050) return 6;\n"
    "    cc64_write(1, \"R\", 1);\n"
    "    return 9;\n"
    "}\n"
)

FLOAT_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    # A binary32 constant narrowed and widened has to be the same number as
    # the binary64 spelling of it, so a constant that lost its high half
    # fails here rather than comparing equal to everything else.
    "static int near(float a, float b)\n"
    "{\n"
    "    float difference = a - b;\n"
    "    if (difference < 0.0f) difference = -difference;\n"
    "    return difference < 0.0001f;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "    float one = 1.0f;\n"
    "    float two = 2.0f;\n"
    "    double wide = 2.5;\n"
    "    if (!(one < two)) return 1;\n"
    "    if (!(two > one)) return 2;\n"
    "    if (!(one <= one)) return 3;\n"
    "    if (!(one >= one)) return 4;\n"
    "    if (one == two) return 5;\n"
    "    if (!(one != two)) return 6;\n"
    "    if (two > wide) return 7;\n"
    "    if (wide < two) return 8;\n"
    "    if (one + two != 3.0f) return 9;\n"
    "    if (two * one != 2.0f) return 10;\n"
    "    if (two / one != 2.0f) return 11;\n"
    "    if (wide / 2.0 != 1.25) return 12;\n"
    "    if (!near(one, 1.0f)) return 13;\n"
    "    if (!near(2.5f, 2.5f)) return 14;\n"
    "    if (one == 0.0f) return 15;\n"
    "    if (sizeof(float) != 4) return 16;\n"
    "    if (sizeof(double) != 8) return 17;\n"
    "    if (_Alignof(float) != 4) return 18;\n"
    "    cc64_write(1, \"F\", 1);\n"
    "    return 9;\n"
    "}\n"
)

# A static assertion produces no code, so a case that only compiled would not
# notice a wrong evaluation. This one checks at run time that the assertion's
# own arithmetic, `__func__`, and `_Alignof` all agree with the ABI the
# compiler wrote, in a file-scope and a block-scope position, and that a
# function name object is distinct per function.
ASSERT_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static_assert(sizeof(char) == 1, \"char is one byte\");\n"
    "static_assert(sizeof(int) == 4 && sizeof(long) == 8, \"int and long\");\n"
    "static_assert(_Alignof(long) == 8, \"long alignment\");\n"
    "static const char *name_of(void) { return __func__; }\n"
    "int main(void)\n"
    "{\n"
    "    static_assert(sizeof(short) == 2, \"short is two bytes\");\n"
    "    if (_Alignof(int) != 4 || _Alignof(char) != 1) return 1;\n"
    "    const char *self = __func__;\n"
    "    const char *other = name_of();\n"
    "    if (self[0] != 'm' || self[1] != 'a' || self[2] != 'i' || self[3] != 'n') return 2;\n"
    "    if (self[4] != 0) return 3;\n"
    # Two functions must not share one name object, or the second would read
    # the first's name.
    "    if (other[0] != 'n' || other[1] != 'a' || other[2] != 'm' || other[3] != 'e') return 4;\n"
    "    if (other[6] != 'f' || other[7] != 0) return 5;\n"
    "    if (self == other) return 6;\n"
    "    cc64_write(1, self, 4);\n"
    "    cc64_write(1, \"\\n\", 1);\n"
    "    return 9;\n"
    "}\n"
)

# The command tail is everything the shell was given after the program name,
# and the name itself arrives in argv[0] from the process control block, so
# "C64H AA BB" starts the program with three arguments. This case is the
# observable check of the startup contract: the name, the tail copy, the
# in-place split, and the null terminator after the last entry.
ARGUMENT_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "int main(int argc, char **argv) {\n"
    "  if (argc != 3) return 1;\n"
    "  if (argv[0][0] != \'C\' || argv[0][1] != \'6\' || argv[0][2] != \'4\'\n"
    "      || argv[0][3] != \'H\' || argv[0][4] != 0) return 5;\n"
    "  cc64_write(1, \"a0=\", 3); cc64_write(1, argv[0], 4);\n"
    "  cc64_write(1, \" a1=\", 4); cc64_write(1, argv[1], 2);\n"
    "  cc64_write(1, \" a2=\", 4); cc64_write(1, argv[2], 2);\n"
    "  cc64_write(1, \"\\n\", 1);\n"
    "  if (argv[3] != 0) return 2;\n"
    "  return 9;\n"
    "}\n"
)


LONG_ARGUMENT_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static void putnum(long value) {\n"
    "  char digits[8];\n"
    "  int at = 7;\n"
    "  digits[7] = 0;\n"
    "  if (value == 0L) digits[--at] = '0';\n"
    "  while (value != 0L) {\n"
    "    long rest = value / 10L;\n"
    "    digits[--at] = (char)('0' + (int)(value - rest * 10L));\n"
    "    value = rest;\n"
    "  }\n"
    "  cc64_write(1, digits + at, (unsigned long)(7 - at));\n"
    "}\n"
    "int main(int argc, char **argv) {\n"
    # A full argument vector has to survive the startup frame: the vector and
    # the command text are separate regions, and the count is bounded (D-059).
    "  int i;\n"
    "  int length;\n"
    "  if (argc != 32) return 1;\n"
    "  if (argv[0][0] != 'C' || argv[0][1] != '6' || argv[0][2] != '4'\n"
    "      || argv[0][3] != 'D' || argv[0][4] != 0) return 2;\n"
    "  for (i = 1; i < argc; ++i) {\n"
    "    length = i < 10 ? 1 : 2;\n"
    "    if (argv[i][0] != 'a') return 3;\n"
    "    if (length == 1) {\n"
    "      if (argv[i][1] != (char)('0' + i) || argv[i][2] != 0) return 3;\n"
    "    } else {\n"
    "      if (argv[i][1] != (char)('0' + i / 10)\n"
    "          || argv[i][2] != (char)('0' + i % 10) || argv[i][3] != 0)\n"
    "        return 3;\n"
    "    }\n"
    "  }\n"
    "  if (argv[argc] != 0) return 4;\n"
    "  cc64_write(1, \"argc=\", 5);\n"
    "  putnum((long)argc);\n"
    "  cc64_write(1, \"\\n\", 1);\n"
    "  return 9;\n"
    "}\n"
)



# Aggregate-by-value passing and return. Every shape the ABI document defines
# appears here: one general piece, two general pieces, one vector piece, two
# vector pieces, a piece that is only partly inside the object, an argument
# that runs out of registers and lands in the outgoing area, a result written
# into a caller-supplied destination, and a value read out of a register into
# a callee frame slot (D-100, D-101).
AGGREGATE_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "typedef struct { int x; int y; } Point;\n"
    "typedef struct { int a; int b; int c; int d; } Rect;\n"
    "typedef struct { float a; float b; } Pair;\n"
    "typedef struct { float l; float a; float b; float alpha; } Lab;\n"
    "typedef struct { char c; } Byte;\n"
    "typedef struct { char b[3]; } Triple;\n"
    "static int sum(Point p) { return p.x + p.y; }\n"
    "static Point make(int x, int y) { Point p; p.x = x; p.y = y; return p; }\n"
    "static Rect widen(Rect r, int by) {\n"
    "    r.a -= by; r.b -= by; r.c += by; r.d += by;\n"
    "    return r;\n"
    "}\n"
    "static int area(Rect r) { return (r.c - r.a) * (r.d - r.b); }\n"
    "static double pair_sum(Pair p) { return (double)p.a + (double)p.b; }\n"
    "static Lab scale(Lab v, float k) {\n"
    "    v.l *= k; v.a *= k; v.b *= k; v.alpha *= k;\n"
    "    return v;\n"
    "}\n"
    "static int byte_value(Byte b) { return b.c; }\n"
    "static int triple_sum(Triple t) { return t.b[0] + t.b[1] + t.b[2]; }\n"
    "static int wide(Point a, Point b, Point c, Point d, Point e) {\n"
    "    return a.x + b.x + c.x + d.x + e.x;\n"
    "}\n"
    "int main(void) {\n"
    "    Point p = make(3, 4);\n"
    "    if (sum(p) != 7) return 1;\n"
    "    Rect r = {0, 0, 4, 4};\n"
    "    if (area(r) != 16) return 2;\n"
    "    if (area(widen(r, 1)) != 36) return 3;\n"
    "    Rect s = widen(widen(r, 1), 1);\n"
    "    if (area(s) != 64) return 4;\n"
    "    Pair q = {1.0f, 2.0f};\n"
    "    if (pair_sum(q) != 3.0) return 5;\n"
    "    Lab v = {1.0f, 2.0f, 3.0f, 4.0f};\n"
    "    Lab w = scale(v, 2.0f);\n"
    "    if (w.l != 2.0f || w.alpha != 8.0f) return 6;\n"
    "    Byte y;\n"
    "    y.c = 81;\n"
    "    if (byte_value(y) != 81) return 7;\n"
    "    Triple t;\n"
    "    t.b[0] = 1; t.b[1] = 2; t.b[2] = 3;\n"
    "    if (triple_sum(t) != 6) return 8;\n"
    "    if (wide(make(1, 0), make(2, 0), make(3, 0), make(4, 0), make(5, 0)) != 15)\n"
    "        return 9;\n"
    "    Point n = make(sum(make(1, 1)), sum(make(2, 2)));\n"
    "    if (n.x != 2 || n.y != 4) return 10;\n"
    "    cc64_write(1, \"G\", 1);\n"
    "    return 0;\n"
    "}\n"
)

# A returned expression is converted to the type the function declares, and a
# compound assignment on a floating object works in the floating unit rather
# than on the bits. Both are separate defects the aggregate work exposed
# (D-103, D-104).
RETURN_CONVERSION_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static double add(float a, float b) { return a + b; }\n"
    "static double widen_sum(float a, float b, float c, float d)\n"
    "{\n"
    "    return a + b + c + d;\n"
    "}\n"
    "static double scale_in_place(float *v, float k)\n"
    "{\n"
    "    *v *= k;\n"
    "    return (double)*v;\n"
    "}\n"
    "static double accumulate(float start)\n"
    "{\n"
    "    float total = start;\n"
    "    total += 1.5f;\n"
    "    total -= 0.5f;\n"
    "    total *= 4.0f;\n"
    "    total /= 2.0f;\n"
    "    return (double)total;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "    if (add(1.0f, 2.0f) != 3.0) return 1;\n"
    "    if (widen_sum(1.0f, 2.0f, 3.0f, 4.0f) != 10.0) return 2;\n"
    "    float v = 3.0f;\n"
    "    if (scale_in_place(&v, 2.0f) != 6.0) return 3;\n"
    "    if (v != 6.0f) return 4;\n"
    "    if (accumulate(1.0f) != 4.0) return 5;\n"
    "    cc64_write(1, \"R\", 1);\n"
    "    return 0;\n"
    "}\n"
)

# A one-byte object stored through a register whose number names a legacy high
# register without a REX prefix, which is the defect D-102 describes.
NARROW_REGISTER_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "static unsigned char widen(unsigned char a, unsigned char b)\n"
    "{\n"
    "    return (unsigned char)(a + b);\n"
    "}\n"
    "static short join(short a, short b) { return (short)(a + b); }\n"
    "int main(void)\n"
    "{\n"
    "    unsigned char c = 40;\n"
    "    unsigned char d = 2;\n"
    "    c += d;\n"
    "    if (c != 42) return 1;\n"
    "    if (widen(40, 2) != 42) return 2;\n"
    "    if (join(20, 22) != 42) return 3;\n"
    "    cc64_write(1, \"N\", 1);\n"
    "    return 0;\n"
    "}\n"
)

CASES = [
("C64R", "int main(void) { return 7; }", "raw", 7, None),
("C64S", "int f(int x){int y=0; switch(x){case 7: y=9; break; default: y=3;} return y;} int main(void){return f(7);}", "raw", 9, None),
("C64A", "int main(void){int a[2][2]={{1,2},{3,4}}; return a[1][1];}", "raw", 4, None),
("C64P", "int x=7; int *p=&x; int main(void){return *p;}", "mz64", 7, None),
("C64I", "int main(void){int x=4; int y=x++; return y*10+x;}", "raw", 45, None),
("C64X", "void cc64_exit(int); int main(void){cc64_exit(9); return 3;}", "raw", 9, None),
("C64O", "int cc64_open(const char *); int cc64_close(int); int main(void){int h=cc64_open(\"HELLO.TXT\"); if(h>=0) cc64_close(h); return h>=0?7:1;}", "raw", 7, None),
("C64U", "int cc64_putc(int); int main(void){cc64_putc(65); return 7;}", "raw", 7, "A"),
("C64T", "int cc64_write(int,const void*,unsigned long); int main(void){cc64_write(1,\"W\",1); return 8;}", "raw", 8, "W"),
("C64M", "void *cc64_alloc(unsigned long); void cc64_free(void*); int main(void){char *p=cc64_alloc(16); if(!p)return 1; p[0]=7; int v=p[0]; cc64_free(p); return v;}", "raw", 7, None),
("C64F", "int cc64_open(const char*); int cc64_read(int,void*,unsigned long); int cc64_close(int); int main(void){char b[4]; int h=cc64_open(\"HELLO.TXT\"); if(h<0)return 1; cc64_read(h,b,4); cc64_close(h); return b[0]==72?7:2;}", "raw", 7, None),
("C64B", "int zero_global; int main(void){zero_global=9; return zero_global;}", "raw", 9, None),
("C64Z", "int zero_global; int main(void){zero_global=9; return zero_global;}", "mz64", 9, None),
# With no command tail the vector still holds the program name, so argc is
# one rather than zero.
("C64G", "int main(int argc, char **argv){return argc == 1 ? 9 : 1;}",
 "raw", 9, None),
("C64H", ARGUMENT_PROGRAM, "raw", 9, "a0=C64H a1=AA a2=BB",
 "C64H AA BB"),
("C64D", LONG_ARGUMENT_PROGRAM, "raw", 9, "argc=32",
 "C64D a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 "
 "a16 a17 a18 a19 a20 a21 a22 a23 a24 a25 a26 a27 a28 a29 a30 a31"),
("C64W", FILE_WRITE_PROGRAM, "raw", 7, None),
("C64K", SEEK_PROGRAM, "raw", 7, None),
("C64V", VARARG_PROGRAM, "raw", 9, "s=10 n=138"),
("C64C", COMPARE_PROGRAM, "raw", 9, "42\nK="),
("C64Q", CONDITIONAL_PROGRAM, "raw", 9, "algayes"),
("C64N", INITIALIZER_PROGRAM, "raw", 9, "I"),
("C64E", ASSERT_PROGRAM, "raw", 9, "main"),
("C64Y", FLOAT_PROGRAM, "raw", 9, "F"),
# The same zero-initialised program in the form that has a separate BSS, so
# the loader's zeroing is checked on the image form that actually has one.
("C64G2", BSS_PROGRAM, "mz64", 9, "B"),
("C64J", RECURSION_PROGRAM, "raw", 9, "R"),
("C64Z2", AGGREGATE_PROGRAM, "raw", 0, "G"),
("C64R2", RETURN_CONVERSION_PROGRAM, "raw", 0, "R"),
("C64N2", NARROW_REGISTER_PROGRAM, "raw", 0, "N"),
]

LIBRARY_CASE = "C64L"
LIBRARY_EXIT = 9
LIBRARY_OUTPUT = "A4BZCDk=42:-7E"
LIBRARY_TIMEOUT = 240.0
