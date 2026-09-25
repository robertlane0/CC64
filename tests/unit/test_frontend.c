#include "cc64.h"
#include "frontend/frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

static Source *add_text(SourceManager *manager, const char *text)
{
    return source_manager_add(manager, "frontend-test.c",
                              (const unsigned char *)text, strlen(text));
}

static bool has_token(const TokenList *list, const char *text)
{
    for (size_t i = 0U; i < list->count; ++i) {
        if (strcmp(list->items[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}

static void test_lexer(void)
{
    Arena *arena = arena_create(262144U);
    SourceManager *manager = source_manager_create(arena);
    const char *text = "int x; // hidden \\\nstill hidden\n/* *\\\n/ hidden */ y += 1;\n";
    Source *source = add_text(manager, text);
    DiagnosticSink diagnostics = {0};
    TokenList tokens;
    CHECK(lex_source(arena, source, &diagnostics, &tokens));
    CHECK(tokens.count != 0U);
    CHECK(tokens.items[tokens.count - 1U].kind == TOKEN_EOF);
    CHECK(has_token(&tokens, "int"));
    CHECK(has_token(&tokens, "y"));
    CHECK(has_token(&tokens, "+="));
    CHECK(!has_token(&tokens, "hidden"));
    CHECK(diagnostics.count == 0U);
    token_list_free(&tokens);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_preprocessor(void)
{
    Arena *arena = arena_create(1048576U);
    SourceManager *manager = source_manager_create(arena);
    const char *text =
        "#define VALUE 4\n"
        "#define ADD(a,b) ((a)+(b))\n"
        "#define SELF SELF\n"
        "#if 010 == 8 && '\\n' == 10 && (-1 >> 1) == -1\n"
        "int result = ADD(VALUE, 3);\n"
        "SELF\n"
        "#define LINE __LINE__\n"
        "LINE\n"
        "#else\n"
        "int wrong;\n"
        "#endif\n";
    Source *source = add_text(manager, text);
    DiagnosticSink diagnostics = {0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, true};
    TokenList output;
    token_list_init(&output);
    CHECK(preprocess_source(arena, manager, &diagnostics, source, &options,
                            &output));
    CHECK(diagnostics.count == 0U);
    CHECK(has_token(&output, "4"));
    CHECK(has_token(&output, "result"));
    CHECK(has_token(&output, "SELF"));
    CHECK(!has_token(&output, "wrong"));
    CHECK(output.items[output.count - 1U].kind == TOKEN_EOF);
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

static void test_errors(void)
{
    Arena *arena = arena_create(262144U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = add_text(manager, "#if 1\nint x;\n");
    DiagnosticSink diagnostics = {0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, false};
    TokenList output;
    token_list_init(&output);
    CHECK(!preprocess_source(arena, manager, &diagnostics, source, &options,
                             &output));
    CHECK(diagnostics.count != 0U);
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

int main(void)
{
    test_lexer();
    test_preprocessor();
    test_errors();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test(s) failed\n", failures);
        return 1;
    }
    puts("frontend: lexer/preprocessor groups passed");
    return 0;
}
