#include "backend/encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LabelSlot {
    size_t id;
    size_t offset;
    bool defined;
} LabelSlot;

typedef struct LabelFixup {
    size_t field;
    size_t label;
} LabelFixup;

typedef struct Encoder {
    Arena *arena;
    DiagnosticSink *diagnostics;
    ObjectBuilder *builder;
    ObjectSection *text;
    ObjectSection *data;
    ObjectSection *bss;
    unsigned char *code;
    size_t code_size;
    size_t code_capacity;
    size_t function_offset;
    IrFunction *function;
    Symbol *main_symbol;
    size_t stack_depth;
    LabelSlot *labels;
    size_t label_count;
    size_t label_capacity;
    LabelFixup *fixups;
    size_t fixup_count;
    size_t fixup_capacity;
    bool failed;
} Encoder;

static const unsigned char argument_registers[6] = {7U, 6U, 2U, 1U, 8U, 9U};

static void encoder_error(Encoder *encoder, unsigned id, const SourceLocation *location,
                          const char *message)
{
    diagnostic_emit(encoder->diagnostics, id, DIAG_BACKEND,
                    location == NULL ? NULL : location->source,
                    location == NULL ? 0U : location->line,
                    location == NULL ? 0U : location->column, message);
    encoder->failed = true;
}

static bool ensure_code(Encoder *encoder, size_t extra)
{
    if (extra > SIZE_MAX - encoder->code_size) return false;
    size_t needed = encoder->code_size + extra;
    if (needed > encoder->code_capacity) {
        size_t next = encoder->code_capacity == 0U ? 256U : encoder->code_capacity;
        while (next < needed) {
            if (next > SIZE_MAX / 2U) { next = needed; break; }
            next *= 2U;
        }
        encoder->code = cc64_xrealloc(encoder->code, next);
        encoder->code_capacity = next;
    }
    return true;
}

static void emit8(Encoder *encoder, unsigned char value)
{
    if (!ensure_code(encoder, 1U)) { encoder->failed = true; return; }
    encoder->code[encoder->code_size++] = value;
}

static void emit32(Encoder *encoder, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; ++i) emit8(encoder, (unsigned char)(value >> (i * 8U)));
}

static void emit64(Encoder *encoder, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) emit8(encoder, (unsigned char)(value >> (i * 8U)));
}

static void emit_rex(Encoder *encoder, bool w, unsigned reg, unsigned rm)
{
    unsigned value = (w ? 8U : 0U) | ((reg >= 8U) ? 4U : 0U) | ((rm >= 8U) ? 1U : 0U);
    if (value != 0U) emit8(encoder, (unsigned char)(0x40U | value));
}

static void emit_modrm_reg(Encoder *encoder, unsigned reg, unsigned rm)
{
    emit8(encoder, (unsigned char)(0xc0U | ((reg & 7U) << 3) | (rm & 7U)));
}

static void emit_mem_reg(Encoder *encoder, unsigned base, unsigned reg,
                         int64_t displacement)
{
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        encoder->failed = true;
        return;
    }
    int32_t disp = (int32_t)displacement;
    if (disp >= -128 && disp <= 127) {
        emit8(encoder, (unsigned char)(0x40U | ((reg & 7U) << 3) | (base & 7U)));
        emit8(encoder, (unsigned char)disp);
    } else {
        emit8(encoder, (unsigned char)(0x80U | ((reg & 7U) << 3) | (base & 7U)));
        emit32(encoder, (uint32_t)disp);
    }
}

static void emit_mem_rip(Encoder *encoder, unsigned reg)
{
    emit8(encoder, (unsigned char)(((reg & 7U) << 3) | 5U));
}

/* LEA reg, [base + displacement]. emit_mem_reg only writes the addressing
   bytes, so the REX prefix and opcode are emitted here. */
static void emit_lea_mem(Encoder *encoder, unsigned base, unsigned reg,
                         int64_t displacement)
{
    emit_rex(encoder, true, reg, base);
    emit8(encoder, 0x8dU);
    emit_mem_reg(encoder, base, reg, displacement);
}

static void emit_mov_reg_reg(Encoder *encoder, unsigned dst, unsigned src)
{
    emit_rex(encoder, true, src, dst);
    emit8(encoder, 0x89U);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_mov_reg32_reg(Encoder *encoder, unsigned dst, unsigned src)
{
    if (src >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, 0x89U);
    emit_modrm_reg(encoder, src, dst);
}

/* Opcode 88 is "MOV r/m8, r8": the destination is the r/m operand and the
   source is the reg operand, so the register arguments are passed in the
   opposite order from the 64-bit move. */
static void emit_mov_reg8_reg(Encoder *encoder, unsigned dst, unsigned src)
{
    emit_rex(encoder, false, src, dst);
    emit8(encoder, 0x88U);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_mov_reg_imm(Encoder *encoder, unsigned reg, uint64_t value, unsigned width)
{
    if (width == 8U) {
        emit8(encoder, (unsigned char)(0x48U | (reg >= 8U ? 1U : 0U)));
        emit8(encoder, (unsigned char)(0xb8U + (reg & 7U)));
        emit64(encoder, value);
    } else {
        if (reg >= 8U) emit8(encoder, 0x41U);
        emit8(encoder, (unsigned char)(0xb8U + (reg & 7U)));
        emit32(encoder, (uint32_t)value);
    }
}

static void emit_push(Encoder *encoder, unsigned reg)
{
    if (reg >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, (unsigned char)(0x50U + (reg & 7U)));
}

static void emit_pop(Encoder *encoder, unsigned reg)
{
    if (reg >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, (unsigned char)(0x58U + (reg & 7U)));
}

static void emit_alu(Encoder *encoder, unsigned opcode, unsigned dst, unsigned src)
{
    emit_rex(encoder, true, src, dst);
    emit8(encoder, (unsigned char)opcode);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_test(Encoder *encoder, unsigned reg)
{
    emit_rex(encoder, true, reg, reg);
    emit8(encoder, 0x85U);
    emit_modrm_reg(encoder, reg, reg);
}

static void emit_setcc(Encoder *encoder, unsigned condition)
{
    emit8(encoder, 0x0fU);
    emit8(encoder, (unsigned char)(0x90U + condition));
    emit8(encoder, 0xc0U);
}

static bool condition_code(CompareOperator compare, bool is_signed, unsigned *code)
{
    switch (compare) {
    case COMPARE_EQUAL: *code = 4U; return true;
    case COMPARE_NOT_EQUAL: *code = 5U; return true;
    case COMPARE_LESS: *code = is_signed ? 12U : 2U; return true;
    case COMPARE_LESS_EQUAL: *code = is_signed ? 14U : 6U; return true;
    case COMPARE_GREATER: *code = is_signed ? 7U : 3U; return true;
    case COMPARE_GREATER_EQUAL: *code = is_signed ? 13U : 5U; return true;
    }
    *code = 0U;
    return false;
}

static void emit_load_mem(Encoder *encoder, unsigned reg, unsigned base, int64_t displacement,
                          size_t width, bool is_signed)
{
    if (width == 1U || width == 2U) {
        emit_rex(encoder, true, reg, base);
        emit8(encoder, 0x0fU);
        emit8(encoder, is_signed ? (width == 1U ? 0xbeU : 0xbfU) :
                      (width == 1U ? 0xb6U : 0xb7U));
    } else if (width == 4U && is_signed) {
        emit_rex(encoder, true, reg, base);
        emit8(encoder, 0x63U);
    } else {
        emit_rex(encoder, width == 8U, reg, base);
        emit8(encoder, 0x8bU);
    }
    emit_mem_reg(encoder, base, reg, displacement);
}

static void emit_store_mem(Encoder *encoder, unsigned src, unsigned base, int64_t displacement,
                           size_t width)
{
    if (width == 2U) emit8(encoder, 0x66U);
    emit_rex(encoder, width == 8U, src, base);
    emit8(encoder, width == 1U ? 0x88U : 0x89U);
    emit_mem_reg(encoder, base, src, displacement);
}

static size_t add_label(Encoder *encoder, size_t id)
{
    for (size_t i = 0U; i < encoder->label_count; ++i) {
        if (encoder->labels[i].id == id) return i;
    }
    if (encoder->label_count == encoder->label_capacity) {
        size_t next = encoder->label_capacity == 0U ? 32U : encoder->label_capacity * 2U;
        LabelSlot *labels = cc64_xrealloc(encoder->labels, next * sizeof(*labels));
        encoder->labels = labels;
        encoder->label_capacity = next;
    }
    size_t index = encoder->label_count++;
    encoder->labels[index].id = id;
    encoder->labels[index].offset = 0U;
    encoder->labels[index].defined = false;
    return index;
}

static void add_fixup(Encoder *encoder, size_t field, size_t label)
{
    if (encoder->fixup_count == encoder->fixup_capacity) {
        size_t next = encoder->fixup_capacity == 0U ? 32U : encoder->fixup_capacity * 2U;
        LabelFixup *fixups = cc64_xrealloc(encoder->fixups, next * sizeof(*fixups));
        encoder->fixups = fixups;
        encoder->fixup_capacity = next;
    }
    encoder->fixups[encoder->fixup_count].field = field;
    encoder->fixups[encoder->fixup_count].label = label;
    ++encoder->fixup_count;
}

static void emit_jump(Encoder *encoder, size_t label)
{
    emit8(encoder, 0xe9U);
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    add_fixup(encoder, field, label);
}

static void emit_conditional_jump(Encoder *encoder, unsigned condition, size_t label)
{
    emit8(encoder, 0x0fU);
    emit8(encoder, (unsigned char)(0x80U + condition));
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    add_fixup(encoder, field, label);
}

static bool resolve_labels(Encoder *encoder)
{
    for (size_t i = 0U; i < encoder->fixup_count; ++i) {
        LabelFixup *fixup = &encoder->fixups[i];
        size_t index = add_label(encoder, fixup->label);
        if (!encoder->labels[index].defined) {
            encoder_error(encoder, 4010U, NULL, "unresolved IR label");
            return false;
        }
        int64_t displacement = (int64_t)encoder->labels[index].offset -
                               (int64_t)(fixup->field + 4U);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            encoder_error(encoder, 4011U, NULL, "branch displacement overflow");
            return false;
        }
        uint32_t encoded = (uint32_t)displacement;
        encoder->code[fixup->field] = (unsigned char)encoded;
        encoder->code[fixup->field + 1U] = (unsigned char)(encoded >> 8);
        encoder->code[fixup->field + 2U] = (unsigned char)(encoded >> 16);
        encoder->code[fixup->field + 3U] = (unsigned char)(encoded >> 24);
    }
    return true;
}

static void define_label(Encoder *encoder, size_t id)
{
    size_t index = add_label(encoder, id);
    encoder->labels[index].offset = encoder->code_size;
    encoder->labels[index].defined = true;
}

static void emit_rip_reference(Encoder *encoder, unsigned opcode, unsigned reg, Symbol *symbol)
{
    emit_rex(encoder, true, reg, 0U);
    emit8(encoder, (unsigned char)opcode);
    emit_mem_rip(encoder, reg);
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    (void)object_add_relocation(encoder->builder, encoder->text,
                                encoder->function_offset + field, symbol,
                                CC64O_REL_PC32, 0, 4U);
}

static size_t object_symbol_index(Encoder *encoder, Symbol *symbol)
{
    for (size_t i = 0U; i < encoder->builder->symbol_count; ++i) {
        ObjectSymbol *entry = &encoder->builder->symbols[i];
        if ((entry->source_symbol == symbol) ||
            (entry->source_symbol == NULL && symbol != NULL &&
             strcmp(entry->name, symbol->name) == 0)) return i;
    }
    unsigned char kind = symbol != NULL && symbol->class == SYMBOL_FUNCTION ? 2U : 1U;
    unsigned char binding = (unsigned char)(symbol != NULL && symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U);
    if (!object_add_symbol(encoder->builder, symbol == NULL ? "" : symbol->name,
                           0U, UINT32_MAX, binding, kind, 0U, false)) return UINT32_MAX;
    encoder->builder->symbols[encoder->builder->symbol_count - 1U].source_symbol = symbol;
    return encoder->builder->symbol_count - 1U;
}

static bool symbol_is_local(const Symbol *symbol)
{
    return symbol != NULL && symbol->has_frame_offset &&
           symbol->storage != STORAGE_STATIC && symbol->storage != STORAGE_EXTERN;
}

static void encode_expr(Encoder *encoder, IrInst *inst);
static bool is_float_type(const Type *type);
static void emit_normalize(Encoder *encoder, size_t width, bool is_signed);
static void emit_load_rax(Encoder *encoder, size_t width, bool is_signed);

static void emit_save_xmm0(Encoder *encoder, size_t width)
{
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x11U); emit8(encoder, 0x04U); emit8(encoder, 0x24U);
    } else {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x11U); emit8(encoder, 0x04U); emit8(encoder, 0x24U);
    }
}

static void emit_restore_xmm(Encoder *encoder, unsigned index, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U);
        emit8(encoder, (unsigned char)(0x04U | (index << 3U)));
        emit8(encoder, 0x24U);
    } else {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U);
        emit8(encoder, (unsigned char)(0x04U | (index << 3U)));
        emit8(encoder, 0x24U);
    }
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 0x08U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
}

static bool call_argument_location(const IrInst *args, const IrInst *target,
                                    unsigned *reg, bool *is_fp)
{
    unsigned integer = 0U;
    unsigned floating = 0U;
    for (const IrInst *arg = args; arg != NULL; arg = arg->next) {
        bool fp = is_float_type(arg->type);
        if (arg == target) {
            if (fp) {
                if (floating >= 8U) return false;
                *reg = floating;
            } else {
                if (integer >= 6U) return false;
                *reg = integer;
            }
            *is_fp = fp;
            return true;
        }
        if (fp) ++floating;
        else ++integer;
    }
    return false;
}

static void encode_call(Encoder *encoder, IrInst *inst)
{
    size_t argument_count = 0U;
    size_t stack_argument_count = 0U;
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) {
        unsigned reg = 0U;
        bool fp = false;
        if (!call_argument_location(inst->args, arg, &reg, &fp)) ++stack_argument_count;
        ++argument_count;
    }
    if (argument_count > 64U) {
        encoder_error(encoder, 4020U, &inst->location,
                      "call argument limit exceeded");
        return;
    }
    bool direct = inst->a != NULL && inst->a->op == IR_ADDR && inst->a->symbol != NULL;
    /* __cc64_va_start is a compiler builtin: it yields the first unnamed
       integer argument slot of the enclosing variadic function. */
    if (direct && argument_count == 0U && inst->a->symbol != NULL &&
        inst->a->symbol->name != NULL &&
        strcmp(inst->a->symbol->name, "__cc64_va_start") == 0) {
        IrFunction *current = encoder->function;
        if (current == NULL || !current->symbol->type->variadic ||
            current->va_area_offset == 0) {
            encoder_error(encoder, 4021U, &inst->location,
                          "va_start used outside a variadic function");
            return;
        }
        unsigned named = 0U;
        for (Symbol *parameter = current->symbol->type->parameters;
             parameter != NULL; parameter = parameter->next) {
            if (!is_float_type(parameter->type) && named < 6U) ++named;
        }
        emit_rex(encoder, true, 0U, 5U);
        emit8(encoder, 0x8dU);
        emit_mem_reg(encoder, 5U, 0U,
                     current->va_area_offset + (int64_t)named * 8);
        return;
    }
    bool adjust = ((encoder->stack_depth + stack_argument_count) & 1U) != 0U;
    if (adjust) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 8U);
        ++encoder->stack_depth;
    }
    for (size_t index = argument_count; index > 0U; --index) {
        IrInst *arg = inst->args;
        for (size_t j = 1U; j < index; ++j) arg = arg == NULL ? NULL : arg->next;
        unsigned reg = 0U;
        bool fp = false;
        if (arg == NULL || call_argument_location(inst->args, arg, &reg, &fp)) continue;
        encode_expr(encoder, arg);
        if (fp) emit_save_xmm0(encoder, type_size(arg->type));
        else { emit_push(encoder, 0U); ++encoder->stack_depth; }
    }
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) {
        unsigned reg = 0U;
        bool fp = false;
        if (!call_argument_location(inst->args, arg, &reg, &fp)) continue;
        if (!fp && reg >= 6U) continue;
        if (fp && reg >= 8U) continue;
        encode_expr(encoder, arg);
        if (fp) emit_save_xmm0(encoder, type_size(arg->type));
        else { emit_push(encoder, 0U); ++encoder->stack_depth; }
    }
    if (!direct) {
        encode_expr(encoder, inst->a);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
        emit_pop(encoder, 11U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
    }
    IrInst *args[64];
    size_t saved_count = 0U;
    for (IrInst *arg = inst->args; arg != NULL && saved_count < 64U; arg = arg->next) {
        unsigned reg = 0U;
        bool fp = false;
        if (call_argument_location(inst->args, arg, &reg, &fp) &&
            ((fp && reg < 8U) || (!fp && reg < 6U))) args[saved_count++] = arg;
    }
    while (saved_count > 0U) {
        IrInst *arg = args[--saved_count];
        unsigned reg = 0U;
        bool fp = false;
        (void)call_argument_location(inst->args, arg, &reg, &fp);
        if (fp) emit_restore_xmm(encoder, reg, type_size(arg->type));
        else { emit_pop(encoder, argument_registers[reg]); if (encoder->stack_depth != 0U) --encoder->stack_depth; }
    }
    /* The calling convention requires AL to report how many vector registers
       carry arguments. It is set after the argument registers are loaded and
       before the call, because AL shares RAX with the result. Version 1
       passes no floating variadic arguments. */
    if (direct && inst->a->symbol != NULL && inst->a->symbol->type != NULL &&
        inst->a->symbol->type->variadic) {
        emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    }
    if (direct) {
        emit8(encoder, 0xe8U);
        size_t field = encoder->code_size;
        emit32(encoder, 0U);
        if (!object_add_relocation(encoder->builder, encoder->text,
                                   encoder->function_offset + field,
                                   inst->a->symbol, CC64O_REL_PC32, 0, 4U)) {
            encoder_error(encoder, 4020U, &inst->location,
                          "cannot record call relocation");
        }
    } else {
        emit_rex(encoder, false, 2U, 11U);
        emit8(encoder, 0xffU);
        emit_modrm_reg(encoder, 2U, 11U);
    }
    size_t stack_argument_bytes = stack_argument_count * 8U;
    if (stack_argument_bytes != 0U) {
        if (stack_argument_bytes <= 127U) {
            emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U);
            emit8(encoder, (unsigned char)stack_argument_bytes);
        } else {
            emit8(encoder, 0x48U); emit8(encoder, 0x81U); emit8(encoder, 0xc4U);
            emit32(encoder, (uint32_t)stack_argument_bytes);
        }
        encoder->stack_depth -= stack_argument_count;
    }
    if (adjust) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 8U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
    }
    if (inst->type != NULL && !is_float_type(inst->type)) {
        emit_normalize(encoder, type_size(inst->type), type_is_signed(inst->type));
    }
}

static bool is_float_type(const Type *type)
{
    return type != NULL && (type->kind == TYPE_FLOAT || type->kind == TYPE_DOUBLE);
}

static void emit_xmm_load_rax(Encoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U); emit8(encoder, 0x00U);
    } else {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U); emit8(encoder, 0x00U);
    }
}

static void emit_xmm_move_from_rax(Encoder *encoder)
{
    emit8(encoder, 0x66U); emit8(encoder, 0x48U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x6eU);
    emit8(encoder, 0xc0U);
}

static void emit_xmm_move_to_rax(Encoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x2cU); emit8(encoder, 0xc0U);
    } else {
        emit8(encoder, 0xf2U); emit8(encoder, 0x48U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x2cU);
        emit8(encoder, 0xc0U);
    }
}

static void emit_xmm_from_stack(Encoder *encoder)
{
    emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x10U); emit8(encoder, 0x04U);
    emit8(encoder, 0x24U);
}

static void emit_xmm_to_stack(Encoder *encoder)
{
    emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x11U); emit8(encoder, 0x04U);
    emit8(encoder, 0x24U);
}

static void encode_float_constant(Encoder *encoder, IrInst *inst)
{
    if (inst->type != NULL && inst->type->kind == TYPE_FLOAT) {
        float value = (float)inst->floating;
        uint32_t bits = 0U;
        memcpy(&bits, &value, sizeof(bits));
        emit_mov_reg_imm(encoder, 0U, bits, 8U);
    } else {
        uint64_t bits = 0U;
        double value = inst->floating;
        memcpy(&bits, &value, sizeof(bits));
        emit_mov_reg_imm(encoder, 0U, bits, 8U);
    }
    emit_xmm_move_from_rax(encoder);
}

static void encode_load(Encoder *encoder, IrInst *inst)
{
    if (is_float_type(inst->type)) {
        if (inst->a == NULL && inst->symbol != NULL) {
            if (symbol_is_local(inst->symbol)) {
                emit_rex(encoder, true, 0U, 5U);
                emit8(encoder, inst->type->kind == TYPE_FLOAT ? 0xf3U : 0xf2U);
                emit8(encoder, 0x0fU); emit8(encoder, 0x10U);
                emit_mem_reg(encoder, 5U, 0U, (int64_t)inst->symbol->offset);
            } else {
                emit_rex(encoder, true, 0U, 0U);
                emit8(encoder, inst->type->kind == TYPE_FLOAT ? 0xf3U : 0xf2U);
                emit8(encoder, 0x0fU); emit8(encoder, 0x10U);
                emit_mem_rip(encoder, 0U);
                size_t field = encoder->code_size;
                emit32(encoder, 0U);
                (void)object_add_relocation(encoder->builder, encoder->text,
                                            encoder->function_offset + field,
                                            inst->symbol, CC64O_REL_PC32, 0, 4U);
            }
        } else {
            encode_expr(encoder, inst->a);
            emit_xmm_load_rax(encoder, type_size(inst->type));
        }
        return;
    }
    size_t width = inst->width == 0U ? 8U : inst->width;
    if (inst->a == NULL && inst->symbol != NULL) {
        if (symbol_is_local(inst->symbol)) {
            emit_load_mem(encoder, 0U, 5U, (int64_t)inst->symbol->offset, width,
                          inst->is_signed);
        } else {
            if (width == 1U || width == 2U) {
                emit_rex(encoder, true, 0U, 0U);
                emit8(encoder, 0x0fU);
                emit8(encoder, inst->is_signed ? (width == 1U ? 0xbeU : 0xbfU) :
                              (width == 1U ? 0xb6U : 0xb7U));
            } else if (width == 4U) {
                if (inst->is_signed) {
                    emit_rex(encoder, true, 0U, 0U);
                    emit8(encoder, 0x63U);
                } else {
                    emit_rex(encoder, false, 0U, 0U);
                    emit8(encoder, 0x8bU);
                }
            } else {
                emit_rex(encoder, true, 0U, 0U);
                emit8(encoder, 0x8bU);
            }
            emit_mem_rip(encoder, 0U);
            size_t field = encoder->code_size;
            emit32(encoder, 0U);
            (void)object_add_relocation(encoder->builder, encoder->text,
                                        encoder->function_offset + field, inst->symbol,
                                        CC64O_REL_PC32, 0, 4U);
        }
        return;
    }
    encode_expr(encoder, inst->a);
    emit_load_rax(encoder, width, inst->is_signed);
}

static void encode_store(Encoder *encoder, IrInst *inst)
{
    if (is_float_type(inst->type)) {
        encode_expr(encoder, inst->a);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
        encode_expr(encoder, inst->b);
        emit_pop(encoder, 11U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
        if (inst->type->kind == TYPE_FLOAT) {
            emit8(encoder, 0xf3U); emit_rex(encoder, false, 0U, 11U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
            emit_mem_reg(encoder, 11U, 0U, 0);
        } else {
            emit8(encoder, 0xf2U); emit_rex(encoder, false, 0U, 11U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
            emit_mem_reg(encoder, 11U, 0U, 0);
        }
        return;
    }
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    size_t width = inst->width == 0U ? 8U : inst->width;
    emit_store_mem(encoder, 1U, 0U, 0, width);
    emit_mov_reg_reg(encoder, 0U, 1U);
}

static void encode_addr(Encoder *encoder, IrInst *inst)
{
    if (inst->symbol != NULL && inst->a == NULL) {
        if (symbol_is_local(inst->symbol)) {
            emit_rex(encoder, true, 0U, 5U);
            emit8(encoder, 0x8dU);
            emit_mem_reg(encoder, 5U, 0U, (int64_t)inst->symbol->offset);
        } else {
            emit_rip_reference(encoder, 0x8dU, 0U, inst->symbol);
        }
        return;
    }
    encode_expr(encoder, inst->a);
}

static void encode_member(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    if (inst->offset == 0) return;
    if (inst->offset >= 0 && inst->offset <= INT32_MAX) {
        emit_rex(encoder, true, 0U, 0U);
        emit8(encoder, 0x81U);
        emit8(encoder, 0xc0U);
        emit32(encoder, (uint32_t)inst->offset);
    } else {
        emit8(encoder, 0x48U); emit8(encoder, 0xc7U); emit8(encoder, 0xc0U);
        emit32(encoder, (uint32_t)inst->offset);
    }
}

static void emit_binary_opcode(Encoder *encoder, IrOp op, bool is_signed)
{
    switch (op) {
    case IR_ADD: emit_alu(encoder, 0x01U, 0U, 1U); break;
    case IR_SUB: emit_alu(encoder, 0x29U, 0U, 1U); break;
    case IR_MUL: emit_rex(encoder, true, 0U, 1U); emit8(encoder, 0x0fU); emit8(encoder, 0xafU); emit_modrm_reg(encoder, 0U, 1U); break;
    case IR_DIV:
        if (is_signed) { emit8(encoder, 0x48U); emit8(encoder, 0x99U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 7U, 1U); }
        else { emit8(encoder, 0x31U); emit_modrm_reg(encoder, 2U, 2U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 6U, 1U); }
        break;
    case IR_MOD:
        if (is_signed) { emit8(encoder, 0x48U); emit8(encoder, 0x99U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 7U, 1U); emit_mov_reg_reg(encoder, 0U, 2U); }
        else { emit8(encoder, 0x31U); emit_modrm_reg(encoder, 2U, 2U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 6U, 1U); emit_mov_reg_reg(encoder, 0U, 2U); }
        break;
    case IR_SHL: emit8(encoder, 0x48U); emit8(encoder, 0xd3U); emit8(encoder, 0xe0U); break;
    case IR_SHR: emit8(encoder, 0x48U); emit8(encoder, 0xd3U); emit8(encoder, is_signed ? 0xf8U : 0xe8U); break;
    case IR_BIT_AND: emit_alu(encoder, 0x21U, 0U, 1U); break;
    case IR_BIT_OR: emit_alu(encoder, 0x09U, 0U, 1U); break;
    case IR_BIT_XOR: emit_alu(encoder, 0x31U, 0U, 1U); break;
    default: break;
    }
}

static void emit_xmm_convert_to_double(Encoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x5aU); emit8(encoder, 0xc0U);
    }
}

static void emit_xmm_convert_from_double(Encoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x5aU); emit8(encoder, 0xc0U);
    }
}

static void encode_float_binary(Encoder *encoder, IrInst *inst)
{
    size_t width = inst->type == NULL ? 8U : type_size(inst->type);
    encode_expr(encoder, inst->a);
    emit_xmm_convert_to_double(encoder, inst->a == NULL ? 8U : type_size(inst->a->type));
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    emit_xmm_to_stack(encoder);
    encode_expr(encoder, inst->b);
    emit_xmm_convert_to_double(encoder, inst->b == NULL ? 8U : type_size(inst->b->type));
    emit8(encoder, 0x66U); emit8(encoder, 0x0fU); emit8(encoder, 0x6fU); emit8(encoder, 0xc8U);
    emit_xmm_from_stack(encoder);
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 0x08U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    switch (inst->op) {
    case IR_ADD: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x58U); emit8(encoder, 0xc1U); break;
    case IR_SUB: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x5cU); emit8(encoder, 0xc1U); break;
    case IR_MUL: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x59U); emit8(encoder, 0xc1U); break;
    case IR_DIV: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x5eU); emit8(encoder, 0xc1U); break;
    default: break;
    }
    emit_xmm_convert_from_double(encoder, width);
}

static void encode_float_compare(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_xmm_convert_to_double(encoder, inst->a == NULL ? 8U : type_size(inst->a->type));
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    emit_xmm_to_stack(encoder);
    encode_expr(encoder, inst->b);
    emit_xmm_convert_to_double(encoder, inst->b == NULL ? 8U : type_size(inst->b->type));
    emit8(encoder, 0x66U); emit8(encoder, 0x0fU); emit8(encoder, 0x6fU); emit8(encoder, 0xc8U);
    emit_xmm_from_stack(encoder);
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 0x08U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit8(encoder, 0x66U); emit8(encoder, 0x0fU); emit8(encoder, 0x2eU); emit8(encoder, 0xc1U);
    unsigned condition = 0U;
    switch (inst->compare) {
    case COMPARE_EQUAL: condition = 4U; break;
    case COMPARE_NOT_EQUAL: condition = 5U; break;
    case COMPARE_LESS: condition = 2U; break;
    case COMPARE_LESS_EQUAL: condition = 6U; break;
    case COMPARE_GREATER: condition = 7U; break;
    case COMPARE_GREATER_EQUAL: condition = 3U; break;
    }
    emit_setcc(encoder, condition);
    emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
}

static void encode_binary(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_binary_opcode(encoder, inst->op, inst->is_signed);
    emit_normalize(encoder, inst->width == 0U ? 8U : inst->width,
                   inst->is_signed);
}

static IrOp compound_ir_op(BinaryOperator binary)
{
    switch (binary) {
    case BINARY_ADD: return IR_ADD;
    case BINARY_SUBTRACT: return IR_SUB;
    case BINARY_MULTIPLY: return IR_MUL;
    case BINARY_DIVIDE: return IR_DIV;
    case BINARY_REMAINDER: return IR_MOD;
    case BINARY_LEFT_SHIFT: return IR_SHL;
    case BINARY_RIGHT_SHIFT: return IR_SHR;
    case BINARY_BITWISE_AND: return IR_BIT_AND;
    case BINARY_BITWISE_OR: return IR_BIT_OR;
    case BINARY_BITWISE_XOR: return IR_BIT_XOR;
    default: return IR_ADD;
    }
}

static void encode_compound(Encoder *encoder, IrInst *inst)
{
    if (inst->a == NULL || inst->b == NULL) return;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_load_rax(encoder, inst->width == 0U ? 8U : inst->width, inst->is_signed);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_binary_opcode(encoder, compound_ir_op(inst->binary), inst->is_signed);
    emit_normalize(encoder, inst->width == 0U ? 8U : inst->width,
                   inst->is_signed);
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_store_mem(encoder, 0U, 11U, 0,
                   inst->width == 0U ? 8U : inst->width);
}

static void emit_normalize(Encoder *encoder, size_t width, bool is_signed)
{
    if (width == 1U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x0fU);
        emit8(encoder, is_signed ? 0xbeU : 0xb6U); emit8(encoder, 0xc0U);
    } else if (width == 2U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x0fU);
        emit8(encoder, is_signed ? 0xbfU : 0xb7U); emit8(encoder, 0xc0U);
    } else if (width == 4U) {
        if (is_signed) {
            emit8(encoder, 0x48U); emit8(encoder, 0x63U); emit8(encoder, 0xc0U);
        } else {
            emit8(encoder, 0x89U); emit8(encoder, 0xc0U);
        }
    }
}

static void emit_load_rax(Encoder *encoder, size_t width, bool is_signed)
{
    if (width == 1U || width == 2U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x0fU);
        emit8(encoder, is_signed ? (width == 1U ? 0xbeU : 0xbfU) :
                      (width == 1U ? 0xb6U : 0xb7U));
        emit8(encoder, 0x00U);
    } else if (width == 4U) {
        if (is_signed) {
            emit8(encoder, 0x48U); emit8(encoder, 0x63U);
        } else {
            emit8(encoder, 0x8bU);
        }
        emit8(encoder, 0x00U);
    } else {
        emit8(encoder, 0x48U); emit8(encoder, 0x8bU); emit8(encoder, 0x00U);
    }
}

static void emit_add_imm(Encoder *encoder, unsigned reg, int64_t value,
                         size_t width)
{
    (void)width;
    if (value >= -128 && value <= 127) {
        emit_rex(encoder, true, 0U, reg);
        emit8(encoder, 0x83U);
        emit_modrm_reg(encoder, 0U, reg);
        emit8(encoder, (unsigned char)(int8_t)value);
    } else if (value >= INT32_MIN && value <= INT32_MAX) {
        emit_rex(encoder, true, 0U, reg);
        emit8(encoder, 0x81U);
        emit_modrm_reg(encoder, 0U, reg);
        emit32(encoder, (uint32_t)(int32_t)value);
    } else {
        emit_mov_reg_imm(encoder, 1U, (uint64_t)value, 8U);
        emit_rex(encoder, true, 1U, reg);
        emit8(encoder, 0x01U);
        emit_modrm_reg(encoder, 1U, reg);
    }
}

static void encode_logical(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_test(encoder, 0U);
    if (inst->op == IR_LOGICAL_AND) {
        emit_conditional_jump(encoder, 4U, inst->false_label);
        encode_expr(encoder, inst->b);
        emit_test(encoder, 0U);
        emit_setcc(encoder, 5U);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        emit_jump(encoder, inst->end_label);
        define_label(encoder, inst->false_label);
        emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
        define_label(encoder, inst->end_label);
    } else {
        emit_conditional_jump(encoder, 5U, inst->true_label);
        encode_expr(encoder, inst->b);
        emit_test(encoder, 0U);
        emit_setcc(encoder, 5U);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        emit_jump(encoder, inst->end_label);
        define_label(encoder, inst->true_label);
        emit8(encoder, 0xb8U); emit32(encoder, 1U);
        define_label(encoder, inst->end_label);
    }
}

static void encode_copy(Encoder *encoder, IrInst *inst)
{
    if (inst->a == NULL || inst->b == NULL) return;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 6U, 0U);
    emit_pop(encoder, 7U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_mov_reg_imm(encoder, 1U, inst->immediate, 8U);
    emit8(encoder, 0xf3U); emit8(encoder, 0xa4U);
    emit_mov_reg_reg(encoder, 0U, 7U);
}

static void encode_zero(Encoder *encoder, IrInst *inst)
{
    if (inst->a == NULL) return;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_mov_reg_reg(encoder, 7U, 0U);
    emit_mov_reg_imm(encoder, 1U, inst->immediate, 8U);
    emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    emit8(encoder, 0xf3U); emit8(encoder, 0xaaU);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
}

static void encode_increment(Encoder *encoder, IrInst *inst)
{
    if (inst->a == NULL) {
        encoder_error(encoder, 4024U, &inst->location,
                      "increment requires an address");
        return;
    }
    size_t width = inst->width == 0U ? 8U : inst->width;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_load_rax(encoder, width, inst->is_signed);
    emit_mov_reg_reg(encoder, 10U, 0U);
    emit_add_imm(encoder, 0U, (int64_t)inst->immediate, width);
    emit_normalize(encoder, width, inst->is_signed);
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_store_mem(encoder, 0U, 11U, 0, width);
    if (inst->post) emit_mov_reg_reg(encoder, 0U, 10U);
}

static void encode_switch(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    if (inst->a != NULL && inst->a->width == 4U) {
        if (inst->a->is_signed) {
            emit8(encoder, 0x48U); emit8(encoder, 0x63U); emit8(encoder, 0xc0U);
        } else {
            emit8(encoder, 0x89U); emit8(encoder, 0xc0U);
        }
    }
    for (IrCase *item = inst->cases; item != NULL; item = item->next) {
        emit_mov_reg_imm(encoder, 1U, (uint64_t)item->value, 8U);
        emit_rex(encoder, true, 1U, 0U);
        emit8(encoder, 0x39U);
        emit_modrm_reg(encoder, 1U, 0U);
        emit_conditional_jump(encoder, 4U, item->label);
    }
    emit_jump(encoder, inst->default_label);
}

static void encode_unary(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    switch (inst->op) {
    case IR_NEG: emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 3U, 0U); break;
    case IR_BIT_NOT: emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 2U, 0U); break;
    case IR_LOGICAL_NOT:
        emit_test(encoder, 0U); emit8(encoder, 0x0fU); emit8(encoder, 0x94U); emit8(encoder, 0xc0U);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        break;
    default: break;
    }
    if (inst->op == IR_NEG || inst->op == IR_BIT_NOT) {
        emit_normalize(encoder, inst->width == 0U ? 8U : inst->width,
                       inst->is_signed);
    }
}

static void encode_conditional(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_test(encoder, 0U);
    emit_conditional_jump(encoder, 4U, inst->false_label);
    encode_expr(encoder, inst->b);
    emit_jump(encoder, inst->end_label);
    define_label(encoder, inst->false_label);
    encode_expr(encoder, inst->c);
    define_label(encoder, inst->end_label);
}

static void encode_cast(Encoder *encoder, IrInst *inst)
{
    size_t width = inst->width == 0U ? 8U : inst->width;
    if (is_float_type(inst->type)) {
        encode_expr(encoder, inst->a);
        if (inst->a != NULL && is_float_type(inst->a->type)) {
            emit_xmm_convert_to_double(encoder, type_size(inst->a->type));
        } else {
            emit8(encoder, 0xf2U); emit8(encoder, 0x48U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x2aU); emit8(encoder, 0xc0U);
        }
        emit_xmm_convert_from_double(encoder, width);
        return;
    }
    encode_expr(encoder, inst->a);
    if (inst->a != NULL && is_float_type(inst->a->type)) {
        emit_xmm_convert_to_double(encoder, type_size(inst->a->type));
        emit_xmm_move_to_rax(encoder, width);
    }
    if (width != 8U) emit_normalize(encoder, width, type_is_signed(inst->type));
}

static void encode_comma(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    encode_expr(encoder, inst->b);
}

static void encode_expr(Encoder *encoder, IrInst *inst)
{
    if (inst == NULL || encoder->failed) return;
    switch (inst->op) {
    case IR_CONST: {
        unsigned width = inst->width == 0U ? 8U : (unsigned)inst->width;
        if (width < 8U && inst->is_signed) {
            int64_t signed_value = (int64_t)inst->immediate;
            emit_mov_reg_imm(encoder, 0U, (uint64_t)signed_value, 8U);
        } else {
            emit_mov_reg_imm(encoder, 0U, inst->immediate, width);
        }
        break;
    }
    case IR_FLOAT_CONST: encode_float_constant(encoder, inst); break;
    case IR_LOAD: encode_load(encoder, inst); break;
    case IR_STORE: encode_store(encoder, inst); break;
    case IR_ADDR: encode_addr(encoder, inst); break;
    case IR_MEMBER: encode_member(encoder, inst); break;
    case IR_CALL: encode_call(encoder, inst); break;
    case IR_CAST: encode_cast(encoder, inst); break;
    case IR_COMMA: encode_comma(encoder, inst); break;
    case IR_CONDITIONAL: encode_conditional(encoder, inst); break;
    case IR_SWITCH: encode_switch(encoder, inst); break;
    case IR_INCREMENT: encode_increment(encoder, inst); break;
    case IR_ZERO: encode_zero(encoder, inst); break;
    case IR_COPY: encode_copy(encoder, inst); break;
    case IR_COMPOUND: encode_compound(encoder, inst); break;
    case IR_COMPARE: {
        if (inst->a != NULL && is_float_type(inst->a->type)) {
            encode_float_compare(encoder, inst);
            break;
        }
        encode_expr(encoder, inst->a); emit_push(encoder, 0U); ++encoder->stack_depth;
        encode_expr(encoder, inst->b); emit_mov_reg_reg(encoder, 1U, 0U); emit_pop(encoder, 0U); if (encoder->stack_depth != 0U) --encoder->stack_depth;
        unsigned condition = 0U;
        if (!condition_code(inst->compare, inst->a != NULL && inst->a->type != NULL && type_is_signed(inst->a->type), &condition)) { encoder_error(encoder, 4022U, &inst->location, "invalid comparison"); return; }
        emit8(encoder, 0x48U); emit8(encoder, 0x39U); emit_modrm_reg(encoder, 1U, 0U); emit_setcc(encoder, condition);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        break;
    }
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_BIT_AND: case IR_BIT_OR: case IR_BIT_XOR:
        if (is_float_type(inst->type) &&
            (inst->op == IR_ADD || inst->op == IR_SUB ||
             inst->op == IR_MUL || inst->op == IR_DIV)) {
            encode_float_binary(encoder, inst);
        } else {
            encode_binary(encoder, inst);
        }
        break;
    case IR_LOGICAL_AND: case IR_LOGICAL_OR:
        encode_logical(encoder, inst); break;
    case IR_NEG: case IR_BIT_NOT: case IR_LOGICAL_NOT: encode_unary(encoder, inst); break;
    default: encoder_error(encoder, 4023U, &inst->location, "invalid expression IR opcode"); break;
    }
}

static void encode_return(Encoder *encoder, IrInst *inst)
{
    if (inst->a != NULL) {
        encode_expr(encoder, inst->a);
        if (inst->type != NULL && !type_is_void(inst->type)) {
            emit_normalize(encoder, type_size(inst->type), type_is_signed(inst->type));
        }
    }
    emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit_modrm_reg(encoder, 5U, 4U);
    emit_pop(encoder, 5U);
    emit8(encoder, 0xc3U);
}

static void encode_statement(Encoder *encoder, IrInst *inst)
{
    if (inst == NULL || encoder->failed) return;
    switch (inst->op) {
    case IR_LABEL: define_label(encoder, inst->id); break;
    case IR_JUMP: emit_jump(encoder, inst->id); break;
    case IR_BRANCH:
        encode_expr(encoder, inst->a);
        emit_test(encoder, 0U);
        emit_conditional_jump(encoder, 4U, inst->false_label);
        break;
    case IR_RETURN: encode_return(encoder, inst); break;
    default: encode_expr(encoder, inst); break;
    }
}

static void emit_prologue(Encoder *encoder, IrFunction *function)
{
    emit_push(encoder, 5U);
    emit_mov_reg_reg(encoder, 5U, 4U);
    if (function->frame_size != 0U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x81U); emit8(encoder, 0xecU);
        emit32(encoder, (uint32_t)function->frame_size);
    }
    unsigned floating_index = 0U;
    unsigned integer_index = 0U;
    for (Symbol *parameter = function->symbol->type->parameters; parameter != NULL;
         parameter = parameter->next) {
        if (is_float_type(parameter->type)) {
            if (floating_index >= 8U) break;
            if (parameter->type->kind == TYPE_FLOAT) {
                emit8(encoder, 0xf3U);
            } else {
                emit8(encoder, 0xf2U);
            }
            emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
            emit_mem_reg(encoder, 5U, floating_index, (int64_t)parameter->offset);
            ++floating_index;
        } else {
            if (integer_index >= 6U) break;
            emit_store_mem(encoder, argument_registers[integer_index], 5U,
                           (int64_t)parameter->offset, 8U);
            ++integer_index;
        }
    }
    /* A variadic function also spills the whole integer argument register set
       so that va_start can walk the unnamed arguments. Floating arguments are
       not part of the version 1 variadic contract. */
    if (function->symbol->type->variadic && function->va_area_offset != 0) {
        for (unsigned i = 0U; i < 6U; ++i) {
            emit_store_mem(encoder, argument_registers[i], 5U,
                           function->va_area_offset + (int64_t)i * 8, 8U);
        }
    }
}

static bool prepare_symbols(Encoder *encoder, IrProgram *program)
{
    for (size_t i = 0U; i < program->global_count; ++i) {
        Symbol *symbol = program->global_symbols[i];
        if (symbol == NULL || symbol->name == NULL) continue;
        if (symbol->storage == STORAGE_EXTERN && symbol->initializer == NULL) {
            (void)object_symbol_index(encoder, symbol);
            continue;
        }
        bool initialized = symbol->initializer != NULL;
        uint32_t section_index = initialized ? 1U : 2U;
        (void)object_add_symbol(encoder->builder, symbol->name, 0U, section_index,
                                (unsigned char)(symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U), 1U,
                                type_size(symbol->type), true);
        encoder->builder->symbols[encoder->builder->symbol_count - 1U].source_symbol = symbol;
        if (!initialized) {
            size_t alignment = type_alignment(symbol->type);
            size_t mask = alignment == 0U ? 0U : alignment - 1U;
            size_t offset = (encoder->bss->size + mask) & ~mask;
            size_t size = type_size(symbol->type);
            encoder->bss->size = offset + size;
            encoder->builder->symbols[encoder->builder->symbol_count - 1U].value = offset;
        }
    }
    for (IrFunction *function = program->functions; function != NULL; function = function->next) {
        (void)object_symbol_index(encoder, function->symbol);
        for (size_t i = 0U; i < encoder->builder->symbol_count; ++i) {
            if (strcmp(encoder->builder->symbols[i].name, function->symbol->name) == 0) {
                encoder->builder->symbols[i].section_index = 0U;
                encoder->builder->symbols[i].kind = 2U;
                encoder->builder->symbols[i].binding = (unsigned char)(function->symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U);
                break;
            }
        }
    }
    return true;
}

static Symbol *initializer_address_symbol(AstNode *value)
{
    while (value != NULL && (value->kind == NODE_CAST ||
                             value->kind == NODE_INITIALIZER)) {
        value = value->a;
    }
    if (value == NULL) return NULL;
    if (value->kind == NODE_STRING) return value->literal_symbol;
    if (value->kind == NODE_IDENTIFIER) return value->symbol;
    if (value->kind == NODE_ADDRESS) return initializer_address_symbol(value->a);
    return NULL;
}

static bool constant_integer_value(AstNode *value, uint64_t *result)
{
    if (value == NULL) return false;
    if (value->kind == NODE_INTEGER) { *result = value->unsigned_integer; return true; }
    if (value->kind == NODE_SIZEOF) { *result = value->unsigned_integer; return true; }
    if (value->kind == NODE_IDENTIFIER && value->symbol != NULL &&
        value->symbol->class == SYMBOL_ENUM_CONSTANT) {
        *result = (uint64_t)value->symbol->enum_value;
        return true;
    }
    if (value->kind == NODE_INITIALIZER || value->kind == NODE_CAST) {
        return constant_integer_value(value->a, result);
    }
    if (value->kind == NODE_UNARY) {
        uint64_t operand = 0U;
        if (!constant_integer_value(value->a, &operand)) return false;
        switch (value->unary) {
        case UNARY_PLUS: *result = operand; return true;
        case UNARY_MINUS: *result = 0U - operand; return true;
        case UNARY_BITWISE_NOT: *result = ~operand; return true;
        case UNARY_LOGICAL_NOT: *result = operand == 0U; return true;
        default: return false;
        }
    }
    if (value->kind == NODE_BINARY) {
        uint64_t left = 0U, right = 0U;
        if (!constant_integer_value(value->a, &left) ||
            !constant_integer_value(value->b, &right)) return false;
        switch (value->binary) {
        case BINARY_ADD: *result = left + right; return true;
        case BINARY_SUBTRACT: *result = left - right; return true;
        case BINARY_MULTIPLY: *result = left * right; return true;
        case BINARY_DIVIDE: if (right == 0U) return false; *result = left / right; return true;
        case BINARY_REMAINDER: if (right == 0U) return false; *result = left % right; return true;
        case BINARY_LEFT_SHIFT: *result = left << (right & 63U); return true;
        case BINARY_RIGHT_SHIFT: *result = left >> (right & 63U); return true;
        case BINARY_BITWISE_AND: *result = left & right; return true;
        case BINARY_BITWISE_OR: *result = left | right; return true;
        case BINARY_BITWISE_XOR: *result = left ^ right; return true;
        case BINARY_LOGICAL_AND: *result = left != 0U && right != 0U; return true;
        case BINARY_LOGICAL_OR: *result = left != 0U || right != 0U; return true;
        case BINARY_LESS: *result = left < right; return true;
        case BINARY_LESS_EQUAL: *result = left <= right; return true;
        case BINARY_GREATER: *result = left > right; return true;
        case BINARY_GREATER_EQUAL: *result = left >= right; return true;
        case BINARY_EQUAL: *result = left == right; return true;
        case BINARY_NOT_EQUAL: *result = left != right; return true;
        }
    }
    return false;
}

static bool write_constant_initializer(Encoder *encoder, AstNode *value,
                                       Type *type, unsigned char *bytes,
                                       size_t base, size_t limit,
                                       size_t relocation_origin)
{
    while (value != NULL && (value->kind == NODE_CAST ||
                             (value->kind == NODE_INITIALIZER &&
                              type != NULL && type->kind != TYPE_ARRAY &&
                              type->kind != TYPE_STRUCT && type->kind != TYPE_UNION))) {
        value = value->a;
    }
    if (value == NULL || type == NULL) return true;
    if (value->kind == NODE_INITIALIZER && type->kind == TYPE_ARRAY) {
        /* A string literal may initialize a character array without braces. */
        AstNode *element = value->a;
        if (element != NULL && element->next == NULL &&
            element->kind == NODE_STRING && type->base != NULL &&
            type_size(type->base) == 1U && base <= limit) {
            size_t copy = element->text_length;
            if (copy > limit - base) copy = limit - base;
            if (copy != 0U) memcpy(bytes + base, element->text, copy);
            return true;
        }
        size_t element_size = type_size(type->base);
        size_t index = 0U;
        for (AstNode *item = value->a; item != NULL; item = item->next, ++index) {
            size_t offset = base + index * element_size;
            if (offset > limit || element_size > limit - offset) break;
            if (!write_constant_initializer(encoder, item, type->base, bytes,
                                            offset, limit,
                                            relocation_origin + offset)) return false;
        }
        return true;
    }
    if (value->kind == NODE_INITIALIZER &&
        (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION)) {
        Member *member = type->members;
        for (AstNode *item = value->a; item != NULL && member != NULL;
             item = item->next, member = member->next) {
            size_t offset = base + member->offset;
            if (offset > limit || type_size(member->type) > limit - offset) break;
            if (!write_constant_initializer(encoder, item, member->type, bytes,
                                            offset, limit,
                                            relocation_origin + offset)) return false;
        }
        return true;
    }
    size_t size = type_size(type);
    if (size == 0U) size = 1U;
    if (base > limit || size > limit - base) return true;
    if (type_is_pointer(type) && size == 8U) {
        Symbol *target = initializer_address_symbol(value);
        if (target != NULL) {
            for (size_t byte = 0U; byte < size; ++byte) bytes[base + byte] = 0U;
            (void)object_symbol_index(encoder, target);
            if (!object_add_relocation(encoder->builder, encoder->data,
                                       relocation_origin,
                                       target, CC64O_REL_DATA64, 0, 8U)) {
                return false;
            }
            return true;
        }
    }
    if (value->kind == NODE_FLOAT && is_float_type(type)) {
        if (type->kind == TYPE_FLOAT) {
            float single = (float)value->floating;
            uint32_t bits = 0U;
            memcpy(&bits, &single, sizeof(bits));
            for (size_t byte = 0U; byte < size && byte < 4U; ++byte) {
                bytes[base + byte] = (unsigned char)(bits >> (byte * 8U));
            }
        } else {
            uint64_t bits = 0U;
            double single = value->floating;
            memcpy(&bits, &single, sizeof(bits));
            for (size_t byte = 0U; byte < size && byte < 8U; ++byte) {
                bytes[base + byte] = (unsigned char)(bits >> (byte * 8U));
            }
        }
        return true;
    }
    uint64_t raw = 0U;
    if (constant_integer_value(value, &raw)) {
        for (size_t byte = 0U; byte < size && byte < 8U; ++byte) {
            bytes[base + byte] = (unsigned char)(raw >> (byte * 8U));
        }
        return true;
    }
    if (value->kind == NODE_STRING && type->kind == TYPE_ARRAY) {
        size_t copy = value->text_length < size ? value->text_length : size;
        if (copy != 0U) memcpy(bytes + base, value->text, copy);
        return true;
    }
    encoder_error(encoder, 4030U, NULL,
                  "initializer is not a constant expression");
    return false;
}

static size_t encoder_align_up(size_t value, size_t alignment)
{
    if (alignment <= 1U) return value;
    size_t mask = alignment - 1U;
    if (value > SIZE_MAX - mask) return SIZE_MAX;
    return (value + mask) & ~mask;
}

static bool append_global_data(Encoder *encoder, IrProgram *program)
{
    for (size_t i = 0U; i < program->global_count; ++i) {
        Symbol *symbol = program->global_symbols[i];
        if (symbol == NULL || symbol->initializer == NULL) continue;
        size_t size = type_size(symbol->type);
        if (size == 0U) size = 1U;
        /* Data objects are aligned to at least eight bytes. A data pointer in
           the image must name an eight-byte-aligned object so that the loader
           can satisfy the MZ64 alignment contract with a single aligned store. */
        size_t alignment = type_alignment(symbol->type);
        if (alignment < 8U) alignment = 8U;
        size_t offset = encoder_align_up(encoder->data->size, alignment);
        if (offset == SIZE_MAX) return false;
        unsigned char *bytes = calloc(size, 1U);
        if (bytes == NULL) return false;
        if (!write_constant_initializer(encoder, symbol->initializer,
                                        symbol->type, bytes, 0U, size,
                                        offset)) {
            free(bytes);
            return false;
        }
        if (offset > encoder->data->size) {
            unsigned char *padding = calloc(offset - encoder->data->size, 1U);
            if (padding == NULL) { free(bytes); return false; }
            bool appended = object_append_data(encoder->builder, encoder->data,
                                               padding,
                                               offset - encoder->data->size);
            free(padding);
            if (!appended) { free(bytes); return false; }
        }
        if (!object_append_data(encoder->builder, encoder->data, bytes, size)) { free(bytes); return false; }
        free(bytes);
        for (size_t j = 0U; j < encoder->builder->symbol_count; ++j) {
            if (encoder->builder->symbols[j].source_symbol == symbol ||
                strcmp(encoder->builder->symbols[j].name, symbol->name) == 0) {
                encoder->builder->symbols[j].value = offset;
                encoder->builder->symbols[j].size = size;
                break;
            }
        }
    }
    return true;
}

typedef enum RuntimeFunction {
    RUNTIME_PUTC,
    RUNTIME_WRITE,
    RUNTIME_READ,
    RUNTIME_ALLOC,
    RUNTIME_FREE,
    RUNTIME_OPEN,
    RUNTIME_CREATE,
    RUNTIME_LSEEK,
    RUNTIME_CLOSE,
    RUNTIME_EXIT,
    RUNTIME_START
} RuntimeFunction;

static bool runtime_function_info(const char *name, RuntimeFunction *function)
{
    if (name == NULL) return false;
    if (strcmp(name, "cc64_putc") == 0) *function = RUNTIME_PUTC;
    else if (strcmp(name, "cc64_write") == 0) *function = RUNTIME_WRITE;
    else if (strcmp(name, "cc64_read") == 0) *function = RUNTIME_READ;
    else if (strcmp(name, "cc64_alloc") == 0) *function = RUNTIME_ALLOC;
    else if (strcmp(name, "cc64_free") == 0) *function = RUNTIME_FREE;
    else if (strcmp(name, "cc64_open") == 0) *function = RUNTIME_OPEN;
    else if (strcmp(name, "cc64_create") == 0) *function = RUNTIME_CREATE;
    else if (strcmp(name, "cc64_lseek") == 0) *function = RUNTIME_LSEEK;
    else if (strcmp(name, "cc64_close") == 0) *function = RUNTIME_CLOSE;
    else if (strcmp(name, "cc64_exit") == 0) *function = RUNTIME_EXIT;
    else if (strcmp(name, "cc64_start") == 0) *function = RUNTIME_START;
    else return false;
    return true;
}

/* The target startup routine. The linker trampoline hands over the process
   prefix; this body copies the command tail into its frame, terminates it,
   splits it on spaces and tabs, and calls main with the resulting argument
   vector. Version 1 has no environment block, so the third argument is null.
   The routine is emitted into every image so that an entry path always works
   without a separate runtime object.
 *
 * Register use follows the call-clobbered set only: RAX holds the tail length,
 * RDX the argument vector base, RSI the tail buffer, RCX the copy count, R11
 * the cursor, R8 the argument count, and R10 the process prefix. */
#define CC64_START_ARGUMENTS 16
#define CC64_START_TAIL 144
/* Frame layout below the frame pointer: the command tail copy occupies the
   upper part and the argument vector the lower part, so neither can overwrite
   the other or the saved frame pointer. */
#define CC64_START_TAIL_AT (-0xa0)
#define CC64_START_VECTOR_AT (-0x100)
#define CC64_START_FRAME 0x140
#define CC64_START_SKIP_LENGTH 1U
#define CC64_START_SKIP_SPACE 2U
#define CC64_START_RECORD 3U
#define CC64_START_SCAN 4U
#define CC64_START_SPLIT 5U
#define CC64_START_DONE 6U

static void emit_startup_body(Encoder *encoder, Symbol *main_symbol)
{
    /* The frame holds the argument vector at the bottom and the command tail
       copy above it. Register use stays inside the call-clobbered set: RAX the
       tail length, RDX the vector base, RSI the tail copy, RCX the copy count,
       R11 the cursor, R8 the argument count, and R10 the process prefix. */
    emit_push(encoder, 5U);
    emit_mov_reg_reg(encoder, 5U, 4U);
    emit8(encoder, 0xfcU);                             /* cld */
    emit8(encoder, 0x48U); emit8(encoder, 0x81U); emit8(encoder, 0xecU);
    emit32(encoder, CC64_START_FRAME);                 /* sub rsp, frame */
    emit_mov_reg_reg(encoder, 10U, 7U);                /* mov r10, psp */
    /* The tail length is a single byte in the process prefix, so it must be
       read as a byte rather than as a word or quadword. */
    emit8(encoder, 0x41U); emit8(encoder, 0x0fU); emit8(encoder, 0xb6U);
    emit8(encoder, 0x82U); emit32(encoder, 0xa0U);      /* movzx eax, byte */
    emit8(encoder, 0x48U); emit8(encoder, 0x3dU);
    emit32(encoder, CC64_START_TAIL - 1U);             /* cmp rax, limit */
    emit_conditional_jump(encoder, 6U, CC64_START_SKIP_LENGTH);
    emit_mov_reg_imm(encoder, 0U, CC64_START_TAIL - 1U, 8U);
    define_label(encoder, CC64_START_SKIP_LENGTH);
    /* Copy the command tail into the frame and terminate it. */
    emit_lea_mem(encoder, 5U, 6U, CC64_START_TAIL_AT);   /* lea rsi */
    emit_lea_mem(encoder, 10U, 7U, 0xa1U);             /* lea rdi */
    emit_mov_reg_reg(encoder, 1U, 0U);                 /* mov rcx, rax */
    emit8(encoder, 0xf3U); emit8(encoder, 0xa4U);       /* rep movsb */
    emit8(encoder, 0xc6U); emit8(encoder, 0x04U);
    emit8(encoder, 0x06U); emit8(encoder, 0x00U);       /* byte [rsi+rax] = 0 */
    emit_lea_mem(encoder, 5U, 2U, CC64_START_VECTOR_AT);  /* lea rdx */
    emit8(encoder, 0x45U); emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    emit_lea_mem(encoder, 5U, 11U, CC64_START_TAIL_AT);   /* lea r11 */
    /* Split the copy in place. The terminating NUL ends the scan, so no
       length is tracked and no read can pass the end of the buffer. */
    define_label(encoder, CC64_START_SKIP_SPACE);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x00U);                              /* cmp byte [r11], 0 */
    emit_conditional_jump(encoder, 4U, CC64_START_DONE);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x20U);                              /* cmp byte [r11], ' ' */
    emit_conditional_jump(encoder, 5U, CC64_START_RECORD);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x09U);                              /* cmp byte [r11], tab */
    emit_conditional_jump(encoder, 5U, CC64_START_RECORD);
    emit8(encoder, 0x49U); emit8(encoder, 0xffU); emit8(encoder, 0xc3U);
    emit_jump(encoder, CC64_START_SKIP_SPACE);
    define_label(encoder, CC64_START_RECORD);
    emit8(encoder, 0x4eU); emit8(encoder, 0x89U); emit8(encoder, 0x1cU);
    emit8(encoder, 0xc2U);                             /* mov [rdx+r8*8], r11 */
    emit8(encoder, 0x41U); emit8(encoder, 0x83U); emit8(encoder, 0xf8U);
    emit8(encoder, (unsigned char)CC64_START_ARGUMENTS);
    emit_conditional_jump(encoder, 7U, CC64_START_DONE);
    emit8(encoder, 0x41U); emit8(encoder, 0xffU); emit8(encoder, 0xc0U);
    define_label(encoder, CC64_START_SCAN);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x00U);
    emit_conditional_jump(encoder, 4U, CC64_START_DONE);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x20U);
    emit_conditional_jump(encoder, 4U, CC64_START_SPLIT);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x09U);
    emit_conditional_jump(encoder, 4U, CC64_START_SPLIT);
    emit8(encoder, 0x49U); emit8(encoder, 0xffU); emit8(encoder, 0xc3U);
    emit_jump(encoder, CC64_START_SCAN);
    define_label(encoder, CC64_START_SPLIT);
    emit8(encoder, 0x41U); emit8(encoder, 0xc6U); emit8(encoder, 0x03U);
    emit8(encoder, 0x00U);                              /* byte [r11] = 0 */
    emit8(encoder, 0x49U); emit8(encoder, 0xffU); emit8(encoder, 0xc3U);
    emit_jump(encoder, CC64_START_SKIP_SPACE);
    define_label(encoder, CC64_START_DONE);
    /* C7 takes no register operand, so the reg field stays zero and only the
       B extension for the r8 index is set. */
    emit8(encoder, 0x4aU); emit8(encoder, 0xc7U); emit8(encoder, 0x04U);
    emit8(encoder, 0xc2U); emit32(encoder, 0U);         /* mov [rdx+r8*8], 0 */
    emit8(encoder, 0x4cU); emit8(encoder, 0x89U); emit8(encoder, 0xc7U);
    emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit8(encoder, 0xd6U);
    emit8(encoder, 0x31U); emit8(encoder, 0xd2U);
    emit8(encoder, 0xe8U);                             /* call main */
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    (void)object_symbol_index(encoder, main_symbol);
    if (!object_add_relocation(encoder->builder, encoder->text,
                               encoder->function_offset + field, main_symbol,
                               CC64O_REL_PC32, 0, 4U)) {
        encoder_error(encoder, 4020U, NULL, "cannot record startup relocation");
    }
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit8(encoder, 0xecU);
    emit8(encoder, 0x5dU);
    emit_mov_reg_imm(encoder, 0U, 0x4c00U, 4U);
    emit_mov_reg8_reg(encoder, 0U, 1U);
    emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
    emit8(encoder, 0xc3U);
    (void)resolve_labels(encoder);
}

static void emit_runtime_body(Encoder *encoder, RuntimeFunction function)
{
    switch (function) {
    case RUNTIME_PUTC:
        emit_mov_reg_imm(encoder, 0U, 0x200U, 4U);
        emit_mov_reg_reg(encoder, 2U, 7U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_mov_reg_reg(encoder, 0U, 7U);
        break;
    case RUNTIME_WRITE:
    case RUNTIME_READ:
        emit_push(encoder, 3U);
        emit_mov_reg_imm(encoder, 0U,
                         function == RUNTIME_WRITE ? 0x4000U : 0x3f00U, 4U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_reg(encoder, 1U, 2U);
        emit_mov_reg_reg(encoder, 2U, 6U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_ALLOC:
        emit_push(encoder, 3U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit8(encoder, 0x48U); emit8(encoder, 0x83U);
        emit8(encoder, 0xc3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x48U); emit8(encoder, 0xc1U);
        emit8(encoder, 0xebU); emit8(encoder, 0x04U);
        emit_mov_reg_imm(encoder, 0U, 0x4800U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_FREE:
        emit_push(encoder, 3U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x4900U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_OPEN:
        emit_push(encoder, 3U);
        emit8(encoder, 0x31U); emit8(encoder, 0xd2U);
        emit_mov_reg_reg(encoder, 2U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x3d00U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_CREATE:
        /* cc64_create(path, mode): RDX=path, AH=3Ch, AL=mode.
           The target returns the file descriptor in RAX with CF clear, or an
           error code in RAX with CF set. */
        emit_mov_reg_reg(encoder, 2U, 7U);
        emit_mov_reg_reg(encoder, 8U, 6U);
        emit_mov_reg_imm(encoder, 0U, 0x3c00U, 4U);
        emit_mov_reg8_reg(encoder, 0U, 8U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        break;
    case RUNTIME_LSEEK:
        /* cc64_lseek(handle, offset, origin): RBX=handle, RCX=signed offset,
           AH=42h, AL=origin. The offset is sign-extended into the 64-bit
           register the target reads. */
        emit_mov_reg_reg(encoder, 8U, 2U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_reg(encoder, 0U, 6U);
        emit8(encoder, 0x99U);
        emit_mov_reg_reg(encoder, 1U, 0U);
        emit_mov_reg_imm(encoder, 0U, 0x4200U, 4U);
        emit_mov_reg8_reg(encoder, 0U, 8U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        break;
    case RUNTIME_CLOSE:
        emit_push(encoder, 3U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x3e00U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_EXIT:
        emit_mov_reg32_reg(encoder, 0U, 7U);
        emit8(encoder, 0xb4U); emit8(encoder, 0x4cU);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        break;
    case RUNTIME_START:
        return;
    }
    emit8(encoder, 0xc3U);
}

static bool append_runtime_functions(Encoder *encoder)
{
    static const char *const names[] = {
        "cc64_putc", "cc64_write", "cc64_read", "cc64_alloc",
        "cc64_free", "cc64_open", "cc64_create", "cc64_lseek",
        "cc64_close", "cc64_exit", "cc64_start"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t symbol_index = UINT32_MAX;
        for (size_t j = 0U; j < encoder->builder->symbol_count; ++j) {
            if (strcmp(encoder->builder->symbols[j].name, names[i]) == 0) {
                symbol_index = j;
                break;
            }
        }
        if (symbol_index == UINT32_MAX) {
            /* The startup routine belongs to every image that defines main, so
               it is emitted even when no source names it. */
            if (strcmp(names[i], "cc64_start") != 0) continue;
            if (encoder->main_symbol == NULL) continue;
            if (!object_add_symbol(encoder->builder, names[i], 0U, UINT32_MAX,
                                   1U, 2U, 0U, true)) return false;
            symbol_index = encoder->builder->symbol_count - 1U;
        }
        if (encoder->builder->symbols[symbol_index].section_index != UINT32_MAX) continue;
        RuntimeFunction function = RUNTIME_EXIT;
        (void)runtime_function_info(names[i], &function);
        Encoder runtime = {0};
        runtime.arena = encoder->arena;
        runtime.diagnostics = encoder->diagnostics;
        runtime.builder = encoder->builder;
        runtime.text = encoder->text;
        runtime.function_offset = encoder->text->size;
        if (function == RUNTIME_START) {
            if (encoder->main_symbol == NULL) continue;
            emit_startup_body(&runtime, encoder->main_symbol);
        } else {
            emit_runtime_body(&runtime, function);
        }
        if (runtime.failed ||
            !object_append_data(encoder->builder, encoder->text, runtime.code,
                                runtime.code_size)) {
            free(runtime.code);
            return false;
        }
        free(runtime.code);
        ObjectSymbol *symbol = &encoder->builder->symbols[symbol_index];
        symbol->section_index = 0U;
        symbol->value = runtime.function_offset;
        symbol->size = runtime.code_size;
        symbol->defined = true;
    }
    return true;
}

static bool encode_function(Encoder *encoder, IrFunction *function)
{
    encoder->code = NULL; encoder->code_size = 0U; encoder->code_capacity = 0U;
    encoder->stack_depth = 0U;
    encoder->label_count = 0U; encoder->fixup_count = 0U;
    encoder->function_offset = encoder->text->size;
    encoder->function = function;
    emit_prologue(encoder, function);
    for (IrInst *inst = function->body; inst != NULL; inst = inst->next) encode_statement(encoder, inst);
    if (!encoder->failed) {
        emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit_modrm_reg(encoder, 5U, 4U);
        emit_pop(encoder, 5U); emit8(encoder, 0xc3U);
    }
    if (!resolve_labels(encoder)) return false;
    if (!object_append_data(encoder->builder, encoder->text, encoder->code, encoder->code_size)) return false;
    for (size_t i = 0U; i < encoder->builder->symbol_count; ++i) {
        if (strcmp(encoder->builder->symbols[i].name, function->symbol->name) == 0) {
            encoder->builder->symbols[i].value = encoder->function_offset;
            encoder->builder->symbols[i].size = encoder->code_size;
            break;
        }
    }
    free(encoder->code); encoder->code = NULL; encoder->code_size = 0U; encoder->code_capacity = 0U;
    return !encoder->failed;
}

bool encode_ir_program(Arena *arena, IrProgram *program, ObjectBuilder *builder,
                       DiagnosticSink *diagnostics)
{
    Encoder encoder = {0};
    encoder.arena = arena;
    encoder.diagnostics = diagnostics;
    encoder.builder = builder;
    encoder.text = object_add_section(builder, ".text", 0U, 16U);
    encoder.data = object_add_section(builder, ".data", 2U, 8U);
    encoder.bss = object_add_section(builder, ".bss", 3U, 8U);
    if (encoder.text == NULL || encoder.data == NULL || encoder.bss == NULL) return false;
    encoder.text = &builder->sections[0];
    encoder.data = &builder->sections[1];
    encoder.bss = &builder->sections[2];
    for (IrFunction *entry = program->functions; entry != NULL; entry = entry->next) {
        if (entry->symbol != NULL && entry->symbol->name != NULL &&
            strcmp(entry->symbol->name, "main") == 0) {
            encoder.main_symbol = entry->symbol;
            break;
        }
    }
    if (!prepare_symbols(&encoder, program) || !append_global_data(&encoder, program)) return false;
    for (IrFunction *function = program->functions; function != NULL; function = function->next) {
        if (!encode_function(&encoder, function)) return false;
    }
    if (!append_runtime_functions(&encoder)) return false;
    free(encoder.labels);
    free(encoder.fixups);
    free(encoder.code);
    return !encoder.failed;
}
