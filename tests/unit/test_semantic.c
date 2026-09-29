#include "cc64.h"
#include "frontend/frontend.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

static bool parse_text(Arena *arena, const char *text, TranslationUnit *unit,
                       DiagnosticSink *diagnostics)
{
    SourceManager *manager = source_manager_create(arena);
    Source *source = source_manager_add(manager, "semantic-test.c",
                                         (const unsigned char *)text, strlen(text));
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, true};
    TokenList tokens;
    token_list_init(&tokens);
    bool preprocessed = preprocess_source(arena, manager, diagnostics, source,
                                          &options, &tokens);
    bool parsed = preprocessed && parse_tokens(arena, &tokens, diagnostics, unit);
    token_list_free(&tokens);
    return parsed;
}

static void test_valid(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    const char *text =
        "typedef unsigned long usize;\n"
        "struct Point { int x; int y; };\n"
        "enum Color { RED, GREEN = 4, BLUE };\n"
        "int global = 3;\n"
        "int add(int a, int b) { int local = a + b; return local; }\n"
        "int main(void) { struct Point p; enum Color c = BLUE; long x = 4; int y = (int)x; return add(y, 2); }\n";
    CHECK(parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count == 0U);
    CHECK(unit.has_main);
    CHECK(unit.count >= 3U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_completed_tag(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    const char *text =
        "typedef struct Forward Forward;\n"
        "struct Forward { int value; };\n"
        "int main(void) { Forward item; Forward *pointer = 0; "
        "item.value = 7; if (pointer == (void *)0) return item.value; return 0; }\n";
    CHECK(parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count == 0U);
    CHECK(unit.has_main);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void expect_error(const char *text)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    CHECK(!parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count != 0U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_static_assert(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    /* Both spellings are accepted, the message is optional, and the
       assertion is checked while the unit is parsed. */
    const char *text =
        "static_assert(sizeof(int) == 4, \"int is four bytes\");\n"
        "_Static_assert(sizeof(long) == 8);\n"
        "int main(void) { static_assert(sizeof(char) == 1, \"char\"); return 7; }\n";
    CHECK(parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count == 0U);
    CHECK(unit.has_main);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_func_name(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    /* `__func__` is a static const char array holding the function's own
       name, so a body reads it as an ordinary object. */
    const char *text =
        "int helper(void) { return __func__[0]; }\n"
        "int main(void) { return __func__[0] == 'm' ? 7 : 1; }\n";
    CHECK(parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count == 0U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void expect_error_id(const char *text, unsigned id)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    CHECK(!parse_text(arena, text, &unit, &diagnostics));
    bool found = false;
    for (size_t i = 0U; i < diagnostics.count; ++i) {
        if (diagnostics.items[i].id == id) found = true;
    }
    CHECK(found);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* Recovery reports the independent errors in one unit rather than stopping at
   the first, so a program with several mistakes is fixed in one pass. Each
   error below is in its own declaration, so none of them can be a consequence
   of another. */
static void test_recovery(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    const char *text =
        "int first(void) { return missing_one; }\n"
        "int second(void) { return missing_two; }\n"
        "int third(void) { return missing_three; }\n"
        "int main(void) { return 0; }\n";
    CHECK(!parse_text(arena, text, &unit, &diagnostics));
    CHECK(diagnostics.count >= 3U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* Recovery inside one declaration: an error in the middle of a statement
   must not swallow the declarations that follow it. */
static void test_recovery_in_declaration(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit;
    const char *text =
        "int first(void) { int local = absent; return local; }\n"
        "int second(void) { return 4; }\n"
        "int main(void) { return second(); }\n";
    CHECK(!parse_text(arena, text, &unit, &diagnostics));
    /* The second function is still parsed, so the unit carries it. */
    bool found = false;
    for (size_t i = 0U; i < unit.count; ++i) {
        if (unit.declarations[i] != NULL &&
            unit.declarations[i]->kind == NODE_FUNCTION_DEFINITION &&
            unit.declarations[i]->symbol != NULL &&
            strcmp(unit.declarations[i]->symbol->name, "second") == 0) {
            found = true;
        }
    }
    CHECK(found);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_deferred_diagnostics(void)
{
    /* Every construct the subset document defers has its own stable
       identifier, so a program that uses one is told which construct is out
       of contract rather than receiving a generic parse failure. */
    expect_error_id("static_assert(0, \"false\");\nint main(void) { return 0; }\n", 2041U);
    expect_error_id("static_assert(main);\nint main(void) { return 0; }\n", 2040U);
    expect_error_id("int main(void) { return _Generic(1, int: 2, default: 3); }\n", 2042U);
    expect_error_id("_Thread_local int shared;\nint main(void) { return 0; }\n", 2038U);
    expect_error_id("_Complex double z;\nint main(void) { return 0; }\n", 2037U);
    expect_error_id("int main(void) { return (int)sizeof(long double); }\n", 2039U);
    expect_error_id("int main(void) { return 0; } _Alignof(int);\n", 2031U);
}

int main(void)
{
    test_valid();
    test_completed_tag();
    test_static_assert();
    test_func_name();
    expect_error("int main(void) { return missing; }\n");
    expect_error("int main(void) { int a; int a; return 0; }\n");
    expect_error("int main(void) { int a[n]; return 0; }\n");
    expect_error("struct S { int x : 1; }; int main(void) { return 0; }\n");
    expect_error("int main(void) { _Atomic int x; return 0; }\n");
    test_deferred_diagnostics();
    test_recovery();
    test_recovery_in_declaration();
    if (failures != 0) {
        fprintf(stderr, "%d semantic test(s) failed\n", failures);
        return 1;
    }
    puts("semantic: valid, deferred, and negative groups passed");
    return 0;
}
