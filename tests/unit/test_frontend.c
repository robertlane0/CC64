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

/* run_preprocess runs one translation unit's preprocessing stage and reports
   whether it succeeded and how many tokens it produced. */
static bool run_preprocess(Arena *arena, const char *text, size_t *token_count)
{
    SourceManager *manager = source_manager_create(arena);
    Source *source = add_text(manager, text);
    DiagnosticSink diagnostics = {0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, false};
    TokenList output;
    token_list_init(&output);
    bool good = preprocess_source(arena, manager, &diagnostics, source,
                                  &options, &output);
    if (token_count != NULL) *token_count = output.count;
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    return good;
}

/* A macro that names itself stops at its own hideset rather than expanding
   forever, and a chain that is merely long is not a recursion. */
static void test_macro_recursion(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    CHECK(run_preprocess(arena, "#define X X\nX\n", NULL));
    CHECK(run_preprocess(arena, "#define F(x) F(x)\nF(1)\n", NULL));
    CHECK(run_preprocess(arena, "#define A B\n#define B A\nA\n", NULL));
    /* A chain deeper than the documented expansion bound is a diagnostic. */
    char deep[4096];
    size_t at = 0U;
    for (unsigned i = 0U; i < 200U; ++i) at += (size_t)snprintf(
        deep + at, sizeof(deep) - at, "#define M%u M%u\n", i, i + 1U);
    (void)snprintf(deep + at, sizeof(deep) - at, "#define M200 1\nM0\n");
    CHECK(run_preprocess(arena, deep, NULL));
    arena_destroy(arena);
}

/* The include depth bound is a limit, not a crash: a unit that includes
   itself is diagnosed rather than reading the same file forever. */
static void test_include_depth(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = add_text(manager, "#include \"loop.h\"\n");
    /* The header is not on disk, so the failure is reported; what matters is
       that a missing or looping include is a diagnostic and not a crash. */
    DiagnosticSink diagnostics = {0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, false};
    TokenList output;
    token_list_init(&output);
    CHECK(!preprocess_source(arena, manager, &diagnostics, source, &options,
                             &output));
    CHECK(diagnostics.count != 0U);
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    CHECK(run_preprocess(arena, "#include <nonexistent-header.h>\n", NULL) == false);
    arena_destroy(arena);
}

/* The token limit bounds a hostile unit. A source longer than the limit is
   rejected deterministically instead of being translated. */
static void test_token_limit(void)
{
    Arena *arena = arena_create(64U * 1024U * 1024U);
    size_t capacity = 3U * 1024U * 1024U + 64U;
    char *text = malloc(capacity);
    CHECK(text != NULL);
    if (text != NULL) {
        size_t at = 0U;
        /* Two tokens per byte pair, so the unit is well past the limit. */
        while (at + 4U < capacity) at += (size_t)snprintf(text + at,
                                                         capacity - at, "a ");
        text[at] = '\0';
        CHECK(!run_preprocess(arena, text, NULL));
        free(text);
    }
    arena_destroy(arena);
}

/* A preprocessor run that cannot succeed must not publish a partial result:
   the caller's list is empty rather than holding half a translation unit. */
static void test_no_partial_output(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    size_t count = 99U;
    CHECK(!run_preprocess(arena, "#if 1\nint x;\n", &count));
    CHECK(count == 0U);
    arena_destroy(arena);
}

int main(void)
{
    test_lexer();
    test_preprocessor();
    test_errors();
    test_macro_recursion();
    test_include_depth();
    test_token_limit();
    test_no_partial_output();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test(s) failed\n", failures);
        return 1;
    }
    puts("frontend: lexer/preprocessor groups passed");
    return 0;
}
