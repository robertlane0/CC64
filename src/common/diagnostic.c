#include "cc64.h"

#include <stdio.h>
#include <string.h>

static const char *const phase_names[] = {
    "driver", "lex", "preprocess", "parse", "semantic",
    "ir", "backend", "link", "runtime", "internal"
};

const char *diagnostic_phase_name(DiagnosticPhase phase)
{
    if ((unsigned)phase >= sizeof(phase_names) / sizeof(phase_names[0])) {
        return "internal";
    }
    return phase_names[(unsigned)phase];
}

void diagnostic_emit(DiagnosticSink *sink, unsigned id, DiagnosticPhase phase,
                     const Source *source, size_t line, size_t column,
                     const char *message)
{
    if (sink->count == sink->limit) {
        return;
    }
    if (sink->count == sink->capacity) {
        size_t next = sink->capacity == 0U ? 16U : sink->capacity * 2U;
        Diagnostic *items = cc64_xrealloc(sink->items, next * sizeof(*items));
        sink->items = items;
        sink->capacity = next;
    }
    Diagnostic *item = &sink->items[sink->count++];
    item->id = id;
    item->phase = phase;
    item->source = source;
    item->line = line;
    item->column = column;
    item->message = cc64_xstrdup(message);
}

void diagnostic_print(const Diagnostic *diagnostic, FILE *stream)
{
    if (diagnostic->source == NULL) {
        fprintf(stream, "cc64: CC%04u: %s: %s\n", diagnostic->id,
                diagnostic_phase_name(diagnostic->phase), diagnostic->message);
        return;
    }
    fprintf(stream, "%s:%zu:%zu: CC%04u: %s: %s\n", diagnostic->source->path,
            diagnostic->line, diagnostic->column, diagnostic->id,
            diagnostic_phase_name(diagnostic->phase), diagnostic->message);
}

size_t diagnostic_error_count(const DiagnosticSink *sink)
{
    return sink->count;
}
