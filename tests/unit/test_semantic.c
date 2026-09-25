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

int main(void)
{
    test_valid();
    expect_error("int main(void) { return missing; }\n");
    expect_error("int main(void) { int a; int a; return 0; }\n");
    expect_error("int main(void) { int a[n]; return 0; }\n");
    expect_error("struct S { int x : 1; }; int main(void) { return 0; }\n");
    expect_error("int main(void) { _Atomic int x; return 0; }\n");
    if (failures != 0) {
        fprintf(stderr, "%d semantic test(s) failed\n", failures);
        return 1;
    }
    puts("semantic: valid and negative groups passed");
    return 0;
}
