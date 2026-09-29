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
]

LIBRARY_CASE = "C64L"
LIBRARY_EXIT = 9
LIBRARY_OUTPUT = "A4BZCDk=42:-7E"
LIBRARY_TIMEOUT = 240.0
