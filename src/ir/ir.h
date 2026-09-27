#ifndef CC64_IR_H
/* The variadic save area holds the six integer argument registers followed by
   this many copied stack slots, so a walk of eight bytes per slot reaches the
   arguments the caller had to pass on the stack. */
#define CC64_VARIADIC_SLOTS 8U

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
    uint32_t label;
    struct IrCase *next;
};

/* One lowered instruction. The record is the largest per-instruction structure
   in the compiler and a large unit lowers to tens of thousands of them, so
   every counter and label is 32-bit, the flags share one word, and the
   immediate, the floating value, and the switch table are alternatives that
   only one kind of instruction uses. */
struct IrInst {
    IrOp op;
    Type *type;
    uint32_t width;
    uint32_t compare;
    uint32_t binary;
    uint32_t flags;               /* bit 0 signed, bit 1 postfix */
    uint32_t id;
    uint32_t true_label;
    uint32_t false_label;
    uint32_t end_label;
    uint32_t default_label;
    uint32_t case_count;
    Symbol *symbol;
    int64_t offset;
    union {
        uint64_t immediate;
        double floating;
    } value;
    IrCase *cases;
    IrInst *a;
    IrInst *b;
    IrInst *c;
    IrInst *args;
    IrInst *next;
    SourceLocation location;
};

#define IR_FLAG_SIGNED 1U
#define IR_FLAG_POST 2U

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
    /* Literal names are numbered for the whole program, not per declaration:
       two literals with the same number would give two symbols the same name,
       and the object writer's string table would merge them. */
    size_t literal_count;
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
/* One declaration at a time, so the caller can release a lowered declaration's
   syntax tree before the next one is parsed. */
bool lower_declaration(Arena *arena, const TranslationUnit *unit,
                       AstNode *declaration, DiagnosticSink *diagnostics,
                       IrProgram *program);
void ir_program_free(IrProgram *program);

#endif
