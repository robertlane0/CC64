#ifndef CC64_ENCODER_H
#define CC64_ENCODER_H

#include "backend/object.h"

bool encode_ir_program(Arena *arena, IrProgram *program,
                       ObjectBuilder *builder, DiagnosticSink *diagnostics);

#endif
