#ifndef CC64_LINKER_H
#define CC64_LINKER_H

#include "cc64.h"

typedef enum ImageFormat {
    IMAGE_RAW_COM,
    IMAGE_MZ64
} ImageFormat;

bool link_objects(Arena *arena, const char *const *objects, size_t object_count,
                  const char *output, ImageFormat format,
                  DiagnosticSink *diagnostics);

#endif
