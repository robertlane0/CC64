#ifndef CC64_LINKER_H
#define CC64_LINKER_H

#include "cc64.h"

typedef enum ImageFormat {
    IMAGE_RAW_COM,
    IMAGE_MZ64
} ImageFormat;

/* release_inputs unlinks each object once it has been read, which a volume too
   small to hold the inputs and the image at the same time needs. It is off by
   default because it destroys the caller's files, and the driver's option is
   named for what it does to the volume's free space rather than for the
   option's length: a target command line is a fixed, short buffer. */
bool link_objects(Arena *arena, const char *const *objects, size_t object_count,
                  const char *output, ImageFormat format, bool release_inputs,
                  DiagnosticSink *diagnostics);

#endif
