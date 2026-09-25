#ifndef CC64_IR_H
#define CC64_IR_H

#include "semantic/semantic.h"

typedef enum IrOp {
    IR_CONST,
    IR_FLOAT_CONST,
    IR_LOAD,
    IR_STORE,
    IR_ADDR,
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_MOD,
    IR_SHL,
    IR_SHR,
    IR_BIT_AND,
    IR_BIT_OR,
    IR_BIT_XOR,
    IR_CAST,
    IR_MEMBER,
    IR_NEG,
    IR_BIT_NOT,
    IR_LOGICAL_NOT,
    IR_COMPARE,
    IR_LOGICAL_AND,
    IR_LOGICAL_OR,
    IR_INCREMENT,
    IR_ZERO,
    IR_COPY,
    IR_COMPOUND,
    IR_CONDITIONAL,
    IR_SWITCH,
    IR_COMMA,
    IR_CALL,
    IR_JUMP,
    IR_BRANCH,
    IR_LABEL,
    IR_RETURN,
    IR_VA_START
} IrOp;

typedef enum CompareOperator {
    COMPARE_EQUAL,
    COMPARE_NOT_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} CompareOperator;

typedef struct IrInst IrInst;
typedef struct IrCase IrCase;
struct IrCase {
    uint64_t value;
    size_t label;
    struct IrCase *next;
};

struct IrInst {
    IrOp op;
    Type *type;
    size_t width;
    bool is_signed;
    uint64_t immediate;
    double floating;
    Symbol *symbol;
    int64_t offset;
    CompareOperator compare;
    bool post;
    BinaryOperator binary;
    IrCase *cases;
    size_t case_count;
    size_t default_label;
    IrInst *a;
    IrInst *b;
    IrInst *c;
    IrInst *args;
    IrInst *next;
    size_t id;
    size_t true_label;
    size_t false_label;
    size_t end_label;
    SourceLocation location;
};

typedef struct IrFunction {
    Symbol *symbol;
    IrInst *body;
    size_t frame_size;
    /* Frame offset of the variadic integer register save area. Zero when the
       function is not variadic; negative once offsets are converted. */
    int64_t va_area_offset;
    struct IrFunction *next;
} IrFunction;

typedef struct IrProgram {
    IrFunction *functions;
    IrFunction *function_tail;
    Symbol *globals;
    Symbol **global_symbols;
    size_t global_count;
    size_t global_capacity;
} IrProgram;

typedef struct IrRelocation {
    size_t offset;
    Symbol *symbol;
    uint32_t type;
    int64_t addend;
    uint32_t width;
    struct IrRelocation *next;
} IrRelocation;

typedef struct IrEncodedFunction {
    Symbol *symbol;
    unsigned char *code;
    size_t code_size;
    IrRelocation *relocations;
    size_t frame_size;
    struct IrEncodedFunction *next;
} IrEncodedFunction;

bool lower_translation_unit(Arena *arena, const TranslationUnit *unit,
                            DiagnosticSink *diagnostics, IrProgram *program);
void ir_program_free(IrProgram *program);

#endif
