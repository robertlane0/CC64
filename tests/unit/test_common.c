#include "cc64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

static void test_arena(void)
{
    Arena *arena = arena_create(131072U);
    void *one = arena_alloc(arena, 7U);
    void *two = arena_alloc_array(arena, 4U, 3U);
    CHECK(one != NULL);
    CHECK(two != NULL);
    CHECK((uintptr_t)one % 16U == 0U);
    CHECK((uintptr_t)two % 16U == 0U);
    CHECK(arena_alloc_array(arena, SIZE_MAX, 2U) == NULL);
    arena_destroy(arena);
}

static void test_source(void)
{
    Arena *arena = arena_create(65536U);
    SourceManager *manager = source_manager_create(arena);
    static const unsigned char text[] = {'a', '\n', 0U, 'b'};
    Source *source = source_manager_add(manager, "unit.c", text, sizeof(text));
    CHECK(source != NULL);
    CHECK(source->length == sizeof(text));
    CHECK(source->bytes[sizeof(text)] == 0U);
    CHECK(source_by_id(manager, source->id) == source);
    CHECK(source_by_id(manager, 99U) == NULL);
    arena_destroy(arena);
}

static void test_diagnostic(void)
{
    DiagnosticSink sink = {0};
    sink.limit = 2U;
    diagnostic_emit(&sink, 42U, DIAG_SEMANTIC, NULL, 0U, 0U, "first");
    diagnostic_emit(&sink, 43U, DIAG_BACKEND, NULL, 0U, 0U, "second");
    diagnostic_emit(&sink, 44U, DIAG_LINK, NULL, 0U, 0U, "dropped");
    CHECK(diagnostic_error_count(&sink) == 2U);
    CHECK(strcmp(diagnostic_phase_name(DIAG_PREPROCESS), "preprocess") == 0);
    diagnostic_sink_destroy(&sink);
}

int main(void)
{
    test_arena();
    test_source();
    test_diagnostic();
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("unit: 3 groups passed");
    return 0;
}
