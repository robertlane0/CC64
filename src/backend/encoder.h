#ifndef CC64_ENCODER_H
#define CC64_ENCODER_H

#include "backend/object.h"

/* An encoder is used one function at a time. A translation unit is encoded as it
   is lowered, so the lowered form of one function is never held together with
   the lowered form of the next. That is what keeps a large unit inside the
   target's memory: the front end's only long-lived copy of a function is the
   machine code the encoder emits from it. */
typedef struct IrEncoder IrEncoder;

IrEncoder *ir_encoder_create(Arena *arena, ObjectBuilder *builder,
                             DiagnosticSink *diagnostics);

bool ir_encoder_add_function(IrEncoder *encoder, const IrFunction *function);

/* Emits what a unit needs after every function: the data of the objects the
   unit declares and the runtime routines the program calls. */
bool ir_encoder_finish(IrEncoder *encoder, const IrProgram *program);

void ir_encoder_destroy(IrEncoder *encoder);

#endif
