#include "cc64.h"
#include "frontend/frontend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
    /* A chain of distinct object-like macros is not recursion, and one inside
       the documented bound expands. A chain past the bound is a diagnostic,
       which is the limit doing its job rather than a runaway. Both halves are
       checked because before the directive-end fix the whole unit was consumed
       by the first directive and neither half ran at all. */
    char chain[4096];
    size_t at = 0U;
    for (unsigned i = 0U; i < CC64_PP_MAX_MACRO_DEPTH - 2U; ++i) {
        at += (size_t)snprintf(chain + at, sizeof(chain) - at, "#define M%u M%u\n",
                               i, i + 1U);
    }
    (void)snprintf(chain + at, sizeof(chain) - at, "#define M%u 1\nM0\n",
                   CC64_PP_MAX_MACRO_DEPTH - 2U);
    CHECK(run_preprocess(arena, chain, NULL));
    at = 0U;
    for (unsigned i = 0U; i < CC64_PP_MAX_MACRO_DEPTH + 4U; ++i) {
        at += (size_t)snprintf(chain + at, sizeof(chain) - at, "#define N%u N%u\n",
                               i, i + 1U);
    }
    (void)snprintf(chain + at, sizeof(chain) - at, "#define N%u 1\nN0\n",
                   CC64_PP_MAX_MACRO_DEPTH + 4U);
    CHECK(!run_preprocess(arena, chain, NULL));
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

/* An include chain is resolved on disk, not only reported missing, so the
   chain and the cycle rule are checked against headers that really exist. */
static void test_include_resolution(void)
{
    char directory[64];
    (void)snprintf(directory, sizeof(directory), "cc64-fe-%ld", (long)getpid());
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "FAIL cannot make the include directory\n");
        ++failures;
        return;
    }
    char outer[160];
    char inner[160];
    char cycle[160];
    (void)snprintf(outer, sizeof(outer), "%s/outer.h", directory);
    (void)snprintf(inner, sizeof(inner), "%s/inner.h", directory);
    (void)snprintf(cycle, sizeof(cycle), "%s/cycle.h", directory);
    FILE *stream = fopen(outer, "wb");
    if (stream != NULL) {
        fputs("#ifndef OUTER_H\n#define OUTER_H\n#define OUTER 1\n"
              "#include \"inner.h\"\n#endif\n", stream);
        (void)fclose(stream);
    }
    stream = fopen(inner, "wb");
    if (stream != NULL) {
        fputs("#ifndef INNER_H\n#define INNER_H\n#define INNER 2\n#endif\n",
              stream);
        (void)fclose(stream);
    }
    stream = fopen(cycle, "wb");
    if (stream != NULL) {
        fputs("#include \"cycle.h\"\n", stream);
        (void)fclose(stream);
    }

    /* A header that includes another is resolved, and the guard on each one
       means the chain terminates rather than repeating. */
    Arena *arena = arena_create(1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = add_text(manager, "#include \"outer.h\"\nOUTER + INNER\n");
    DiagnosticSink diagnostics = {0};
    const char *paths[1] = {directory};
    PreprocessorOptions options = {paths, 1U, NULL, 0U, true};
    TokenList output;
    token_list_init(&output);
    CHECK(preprocess_source(arena, manager, &diagnostics, source, &options,
                            &output));
    CHECK(diagnostics.count == 0U);
    CHECK(has_token(&output, "1"));
    CHECK(has_token(&output, "2"));
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);

    /* A header that includes itself is a cycle, and it is a diagnostic
       rather than a read that never ends. */
    arena = arena_create(1024U * 1024U);
    manager = source_manager_create(arena);
    source = add_text(manager, "#include \"cycle.h\"\n");
    diagnostics = (DiagnosticSink){0};
    token_list_init(&output);
    CHECK(!preprocess_source(arena, manager, &diagnostics, source, &options,
                             &output));
    CHECK(diagnostics.count != 0U);
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);

    (void)remove(outer);
    (void)remove(inner);
    (void)remove(cycle);
    (void)rmdir(directory);
}

/* `__LINE__` names the line the token is on, so a diagnostic or a trace built
   from it points at the line the user wrote. */
static void test_predefined_positions(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = add_text(manager, "__LINE__\n\n__LINE__\n");
    DiagnosticSink diagnostics = {0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, false};
    TokenList output;
    token_list_init(&output);
    CHECK(preprocess_source(arena, manager, &diagnostics, source, &options,
                            &output));
    CHECK(diagnostics.count == 0U);
    /* Two lines apart, and the token carries the line it was written on. */
    CHECK(has_token(&output, "1"));
    CHECK(has_token(&output, "3"));
    CHECK(!has_token(&output, "2"));
    for (size_t i = 0U; i < output.count; ++i) {
        if (strcmp(output.items[i].text, "3") == 0) CHECK(output.items[i].line == 3U);
    }
    token_list_free(&output);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* Decoding the string literal a `#` produced gives back the spelling of the
   argument, so that identity is what the cases below state. Comparing the
   decoded value rather than the raw token keeps the expectations readable and
   keeps them independent of how the escapes are spelled. */
static bool decode_c_literal(const char *text, char *out, size_t capacity)
{
    size_t used = 0U;
    size_t i = 1U; /* the opening quote */
    if (text[0] != '"' || text[i] == '\0') return false;
    while (text[i] != '"') {
        char value = text[i];
        if (value == '\\' && text[i + 1] != '\0') ++i;
        if (used + 1U >= capacity) return false;
        out[used++] = text[i++];
    }
    out[used] = '\0';
    return text[i] == '"';
}

/* The `#` operator escapes every quote and backslash it copies, and the buffer
   it builds has to hold the escaped result plus both quotes. Sizing that buffer
   from the unescaped lengths instead runs past its end for any argument holding
   a quote or a backslash, so each case here states the argument, and the
   decoded stringized result must reproduce it. */
static void test_stringize(void)
{
    Arena *arena = arena_create(1024U * 1024U);
    /* One spelling per case. It is written into the unit as the macro argument
       and the decoded stringized result must read back as the same spelling,
       which is the identity the operator is defined to have. */
    struct { const char *spelling; const char *note; } cases[] = {
        {"abc", "one token, nothing to escape"},
        {"a+b", "several tokens, one separator space between each"},
        {"\"q\"", "a quote inside a literal costs a second byte when escaped"},
        {"\"a\\tb\"", "an escape that is not a quote or backslash is copied as is"},
        {"\"a\\\\b\"", "a backslash is escaped, so it costs a second byte"},
        {"f(\"x\\\"y\\\\z\", 2)", "a quote and a backslash in one argument"},
    };
    for (size_t c = 0U; c < sizeof cases / sizeof cases[0]; ++c) {
        char unit[256];
        int at = snprintf(unit, sizeof unit, "#define S(x) #x\nS(%s)\n",
                          cases[c].spelling);
        CHECK(at > 0 && (size_t)at < sizeof unit);
        /* Newline tokens are omitted here on purpose: the directive-end rule
           has to work without them, and a directive that swallowed the rest of
           the unit would make every case below fail rather than pass wrongly. */
        for (unsigned mode = 0U; mode < 2U; ++mode) {
            SourceManager *manager = source_manager_create(arena);
            Source *source = add_text(manager, unit);
            DiagnosticSink diagnostics = {0};
            PreprocessorOptions options = {NULL, 0U, NULL, 0U, mode != 0U};
            TokenList output;
            token_list_init(&output);
            CHECK(preprocess_source(arena, manager, &diagnostics, source,
                                    &options, &output));
            CHECK(diagnostics.count == 0U);
            bool found = false;
            for (size_t i = 0U; i < output.count; ++i) {
                char decoded[512];
                if (output.items[i].kind != TOKEN_STRING) continue;
                if (!decode_c_literal(output.items[i].text, decoded, sizeof decoded)) {
                    CHECK(!"stringized text is not a decodable literal");
                    continue;
                }
                if (strcmp(decoded, cases[c].spelling) == 0) found = true;
                else fprintf(stderr, "  %s: got \"%s\" want \"%s\"\n",
                             cases[c].note, decoded, cases[c].spelling);
            }
            CHECK(found);
            token_list_free(&output);
            diagnostic_sink_destroy(&diagnostics);
        }
    }
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
    test_include_resolution();
    test_predefined_positions();
    test_stringize();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test(s) failed\n", failures);
        return 1;
    }
    puts("frontend: lexer/preprocessor groups passed");
    return 0;
}
