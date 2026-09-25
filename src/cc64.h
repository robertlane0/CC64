#ifndef CC64_H
#define CC64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CC64_VERSION "0.1.0"
#define CC64_TARGET "x86_64-pc-dos64"

typedef struct ArenaBlock ArenaBlock;
typedef struct Arena {
    ArenaBlock *head;
    size_t total;
    size_t limit;
} Arena;

typedef struct Source {
    const char *path;
    unsigned char *bytes;
    size_t length;
    unsigned id;
} Source;

typedef struct SourceManager {
    Arena *arena;
    Source **sources;
    size_t count;
    size_t capacity;
} SourceManager;

typedef enum DiagnosticPhase {
    DIAG_DRIVER,
    DIAG_LEX,
    DIAG_PREPROCESS,
    DIAG_PARSE,
    DIAG_SEMANTIC,
    DIAG_IR,
    DIAG_BACKEND,
    DIAG_LINK,
    DIAG_RUNTIME,
    DIAG_INTERNAL
} DiagnosticPhase;

typedef struct Diagnostic {
    unsigned id;
    DiagnosticPhase phase;
    const Source *source;
    size_t line;
    size_t column;
    const char *message;
} Diagnostic;

typedef struct DiagnosticSink {
    Diagnostic *items;
    size_t count;
    size_t capacity;
    size_t limit;
    bool warnings_are_errors;
} DiagnosticSink;

void *cc64_xmalloc(size_t size);
void *cc64_xrealloc(void *ptr, size_t size);
char *cc64_xstrdup(const char *text);

Arena *arena_create(size_t limit);
void *arena_alloc(Arena *arena, size_t size);
void *arena_alloc_array(Arena *arena, size_t count, size_t size);
void arena_destroy(Arena *arena);

SourceManager *source_manager_create(Arena *arena);
Source *source_manager_load(SourceManager *manager, const char *path);
Source *source_manager_add(SourceManager *manager, const char *path,
                           const unsigned char *bytes, size_t length);
const Source *source_by_id(const SourceManager *manager, unsigned id);

const char *diagnostic_phase_name(DiagnosticPhase phase);
void diagnostic_emit(DiagnosticSink *sink, unsigned id, DiagnosticPhase phase,
                     const Source *source, size_t line, size_t column,
                     const char *message);
void diagnostic_print(const Diagnostic *diagnostic, FILE *stream);
size_t diagnostic_error_count(const DiagnosticSink *sink);

int cc64_main(int argc, char **argv);

#endif
