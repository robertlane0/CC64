/* Source location and diagnostic tests.

A diagnostic is only useful if it points at the place that is wrong, and a
compiler is only deterministic if the same source produces the same bytes every
time. The groups here check both: that a diagnostic carries the line and column
of the token it names, and that the sink that collects them is bounded.
*/

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

/* diagnose preprocesses and parses one source and returns the diagnostics it
   produced. The caller releases the sink when it is finished with them, which
   is the only way the memory the sink grew is freed. */
static DiagnosticSink diagnose(const char *text, size_t *count)
{
    DiagnosticSink sink = {0};
    Arena *arena = arena_create(4U * 1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = source_manager_add(manager, "location.c",
                                        (const unsigned char *)text,
                                        strlen(text));
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, true};
    TokenList tokens;
    token_list_init(&tokens);
    (void)preprocess_source(arena, manager, &sink, source, &options, &tokens);
    /* The diagnostic a user sees comes from the phase that understood the
       construct, so the text is parsed as well as preprocessed. */
    TranslationUnit unit;
    (void)parse_tokens(arena, &tokens, &sink, &unit);
    translation_unit_free(&unit);
    token_list_free(&tokens);
    arena_destroy(arena);
    if (count != NULL) *count = sink.count;
    return sink;
}

/* A diagnostic has to name the line and the column of the token that caused
   it, because that is the only thing a user can act on. */
static void test_diagnostic_location(void)
{
    size_t count = 0U;
    /* The error is on the second line, and the line it reports is the line the
       error is on rather than a line counted from the first error. */
    DiagnosticSink sink = diagnose("int a = 1;\nint b = 2 + ;\n", &count);
    CHECK(count >= 1U);
    if (count >= 1U) {
        CHECK(sink.items[0].line == 2U);
        CHECK(sink.items[0].column != 0U);
        CHECK(sink.items[0].source != NULL);
        CHECK(sink.items[0].message != NULL && sink.items[0].message[0] != '\0');
    }
    diagnostic_sink_destroy(&sink);

    /* The same error text on a later line reports that line, so the position
       is read from the source rather than accumulated. */
    count = 0U;
    sink = diagnose("int a = 1;\nint b = 2;\nint c = 4 + ;\n", &count);
    CHECK(count >= 1U);
    if (count >= 1U) CHECK(sink.items[0].line == 3U);
    diagnostic_sink_destroy(&sink);

    /* A column is reported for the token, not the start of the line, so two
       errors on one line point at different columns. */
    count = 0U;
    sink = diagnose("int a = first; int b = second;\n", &count);
    CHECK(count == 2U);
    if (count == 2U) {
        CHECK(sink.items[0].column != sink.items[1].column);
        CHECK(sink.items[0].line == sink.items[1].line);
    }
    diagnostic_sink_destroy(&sink);
}

/* The printed form carries the same information, so a user reading a log sees
   the location rather than only a machine being able to. */
static void test_diagnostic_print(void)
{
    size_t count = 0U;
    DiagnosticSink sink = diagnose("int main(void) { return missing; }\n", &count);
    CHECK(count == 1U);
    if (count == 1U) {
        /* The form is written to a stream, so the test reads back a temporary
           file rather than assuming a host buffer the target has not. */
        const char *path = "cc64-diagnostic-test.tmp";
        FILE *stream = fopen(path, "wb");
        CHECK(stream != NULL);
        if (stream != NULL) {
            diagnostic_print(&sink.items[0], stream);
            (void)fclose(stream);
            FILE *read_back = fopen(path, "rb");
            CHECK(read_back != NULL);
            if (read_back != NULL) {
                char buffer[512];
                size_t got = fread(buffer, 1U, sizeof(buffer) - 1U, read_back);
                buffer[got] = '\0';
                (void)fclose(read_back);
                (void)remove(path);
                /* The user sees the file, the identifier, the phase, and the
                   position, so a log line is as useful as the terminal. */
                CHECK(strstr(buffer, "location.c") != NULL);
                CHECK(strstr(buffer, "CC2075") != NULL);
                CHECK(strstr(buffer, "semantic") != NULL);
                CHECK(strchr(buffer, '1') != NULL);
            }
        }
    }
    diagnostic_sink_destroy(&sink);
}

/* The sink is bounded: a source that would produce more diagnostics than the
   limit stops at the limit rather than growing without end, which is what
   keeps a hostile source from exhausting memory. */
static void test_diagnostic_limit(void)
{
    DiagnosticSink bounded = {0};
    bounded.limit = 10U;
    for (unsigned i = 0U; i < 40U; ++i) {
        diagnostic_emit(&bounded, 2075U, DIAG_SEMANTIC, NULL, 1U, 1U,
                        "synthetic");
    }
    CHECK(bounded.count == 10U);
    diagnostic_sink_destroy(&bounded);

    /* A sink with no limit set takes the documented default rather than
       growing for ever. */
    DiagnosticSink unbounded = {0};
    for (unsigned i = 0U; i < 150U; ++i) {
        diagnostic_emit(&unbounded, 2075U, DIAG_SEMANTIC, NULL, 1U, 1U,
                        "synthetic");
    }
    CHECK(unbounded.limit == 100U);
    CHECK(unbounded.count == 100U);
    diagnostic_sink_destroy(&unbounded);

    /* Two sinks are independent, so a second run does not see the first
       run's diagnostics. */
    DiagnosticSink first = {0};
    DiagnosticSink second = {0};
    diagnostic_emit(&first, 2075U, DIAG_SEMANTIC, NULL, 1U, 1U, "one");
    CHECK(second.count == 0U);
    CHECK(diagnostic_error_count(&first) == 1U);
    diagnostic_sink_destroy(&first);
    diagnostic_sink_destroy(&second);
}

int main(void)
{
    test_diagnostic_location();
    test_diagnostic_print();
    test_diagnostic_limit();
    if (failures != 0) {
        fprintf(stderr, "%d diagnostic test(s) failed\n", failures);
        return 1;
    }
    puts("diagnostic: location, printed form, and limit groups passed");
    return 0;
}
