/* Probe: narrow down which construct the target gets wrong.

   Four of the thirty test units of the program CC64 compiles fail on the target
   and pass on a host, and each reads its result through a comparison, so the
   question is whether the construct behind the comparison is wrong or the
   comparison is. Every case here prints its own line with the values involved,
   so one boot says which of them is at fault rather than which symptom appears.
*/
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            printf("PROBE FAIL %d: %s\n", __LINE__, #cond);                   \
            ++failed;                                                         \
        }                                                                     \
    } while (0)

typedef struct {
    int kind;
    const char *bytes;
    size_t len;
} expect_t;

static void probe_compound_literal(void)
{
    /* An array compound literal, which is how the two input tests spell their
       expected tokens. */
    expect_t one = { 1, "hello", 5 };
    /* The variable itself first: if its pointer member is already wrong then
       the compound literal is copying faithfully and the fault is in the
       declaration, not here. */
    printf("PROBE variable: kind=%d len=%u byte0=%d r=%d\n", (int)one.kind,
           (unsigned)one.len, (int)one.bytes[0], memcmp(one.bytes, "hello", 5));
    CHECK(one.kind == 1);
    CHECK(one.len == 5U);
    CHECK(memcmp(one.bytes, "hello", 5) == 0);
    const expect_t *table = (const expect_t[]){ one };
    printf("PROBE compound: kind=%d len=%u byte0=%d\n", (int)table[0].kind,
           (unsigned)table[0].len, (int)table[0].bytes[0]);
    CHECK(table[0].kind == 1);
    CHECK(table[0].len == 5U);
    CHECK(memcmp(table[0].bytes, "hello", 5) == 0);

    /* The same array without const, to tell a qualifier from a shape. */
    const expect_t *plain = (expect_t[]){ one };
    printf("PROBE plain: kind=%d len=%u\n", (int)plain[0].kind,
           (unsigned)plain[0].len);
    CHECK(plain[0].kind == 1);
    CHECK(plain[0].len == 5U);

    /* An array of scalars, to tell an array from a struct. */
    int *numbers = (int[]){ 7, 8, 9 };
    printf("PROBE numbers: n0=%d n1=%d\n", numbers[0], numbers[1]);
    CHECK(numbers[0] == 7);
    CHECK(numbers[1] == 8);

    /* A struct literal whose members are written out. */
    expect_t braced = (expect_t){ 2, "bye", 3 };
    printf("PROBE braced: kind=%d len=%u\n", (int)braced.kind, (unsigned)braced.len);
    CHECK(braced.kind == 2);
    CHECK(braced.len == 3U);

    /* A struct literal whose one member comes from an expression. */
    int n = 5;
    expect_t nested = (expect_t){ 3, "x", n };
    printf("PROBE nested: kind=%d len=%u\n", (int)nested.kind, (unsigned)nested.len);
    CHECK(nested.kind == 3);
    CHECK(nested.len == 5U);

    /* An array of structs written out, which is the shape without a variable. */
    const expect_t *listed = (expect_t[]){ { 4, "q", 1 } };
    printf("PROBE listed: kind=%d len=%u\n", (int)listed[0].kind,
           (unsigned)listed[0].len);
    CHECK(listed[0].kind == 4);
    CHECK(listed[0].len == 1U);
}

static void probe_memcmp(void)
{
    char buffer[16];
    memcpy(buffer, "hello", 6);
    printf("PROBE memcmp ascii: r=%d len=%u\n", memcmp(buffer, "hello", 5),
           (unsigned)strlen(buffer));
    CHECK(memcmp(buffer, "hello", 5) == 0);
    CHECK(memcmp(buffer, "hellp", 5) != 0);
    /* Bytes above 127, so a comparison that treats them as signed can differ
       from one that does not. The editor's renders are full of them. */
    static const char high[6] = { (char)0xC3, (char)0x9F, 'a', (char)0xE2, 'b', 0 };
    char copy[6];
    memcpy(copy, high, 6);
    printf("PROBE memcmp high: r=%d b0=%d b1=%d\n", memcmp(copy, high, 5),
           (int)copy[0], (int)copy[1]);
    CHECK(memcmp(copy, high, 5) == 0);
}

struct pair {
    int a;
    int b;
};

static struct pair make(int a, int b)
{
    struct pair p = { a, b };
    return p;
}

static void probe_struct_return(void)
{
    struct pair p = make(3, 4);
    printf("PROBE struct: a=%d b=%d\n", p.a, p.b);
    CHECK(p.a == 3);
    CHECK(p.b == 4);
}

static void probe_print(void)
{
    /* The width and precision of `*` is standard C and the editor's diagnostics
       use it. */
    printf("PROBE printf plain: [%s]\n", "abc");
    printf("PROBE printf star: [%.*s]\n", 2, "abc");
    printf("PROBE printf star0: [%.*s]\n", 0, "abc");
    printf("PROBE printf starneg: [%.*s]\n", -1, "abc");
    printf("PROBE printf wstar: [%*d]\n", 6, 42);
    printf("PROBE printf dot: [%.2s]\n", "abc");
    printf("PROBE printf prec: [%.5d]\n", 42);
    printf("PROBE printf prec0: [%.0d][%.0d]\n", 42, 0);
    printf("PROBE printf zero: [%05d]\n", 42);
    printf("PROBE printf left: [%-5d]\n", 42);
    printf("PROBE printf pad: [%5d]\n", 42);
    CHECK(1);
}

static void probe_char_sign(void)
{
    char c = (char)0xC3;
    printf("PROBE char: is_signed=%d value=%d unsigned=%u\n",
           (c < 0) ? 1 : 0, (int)c, (unsigned)(unsigned char)c);
    CHECK(c < 0);
    CHECK((int)(unsigned char)c == 0xC3);
}

static void probe_concat(void)
{
    const char *s = "ab" "cd"
                    "ef";
    printf("PROBE concat: len=%u [%s]\n", (unsigned)strlen(s), s);
    CHECK(strlen(s) == 6U);
}

int main(void)
{
    probe_compound_literal();
    probe_memcmp();
    probe_struct_return();
    probe_print();
    probe_char_sign();
    probe_concat();
    printf("PROBE done checks=%d failed=%d\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
