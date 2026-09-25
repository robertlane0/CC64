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

static void emit_mov_reg_reg(Encoder *encoder, unsigned dst, unsigned src)
{
    emit_rex(encoder, true, src, dst);
    emit8(encoder, 0x89U);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_mov_reg_imm(Encoder *encoder, unsigned reg, uint64_t value, unsigned width)
{
    if (reg >= 8U) emit8(encoder, 0x41U);
    if (width == 8U) emit8(encoder, (unsigned char)(0xb8U + (reg & 7U)));
    else emit8(encoder, (unsigned char)(0xb8U + (reg & 7U)));
    if (width == 8U) emit64(encoder, value);
    else emit32(encoder, (uint32_t)value);
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
    bool wide = width == 8U;
    if (width == 1U || width == 2U) {
        emit_rex(encoder, false, reg, base);
        emit8(encoder, is_signed ? (width == 1U ? 0x0fU : 0x0fU) : 0x0fU);
        if (is_signed) {
            emit8(encoder, width == 1U ? 0xbeU : 0xbfU);
        } else {
            emit8(encoder, width == 1U ? 0xb6U : 0xb7U);
        }
    } else {
        if (width == 4U && is_signed) {
            emit_rex(encoder, true, reg, base);
            emit8(encoder, 0x63U);
        } else {
            emit_rex(encoder, wide, reg, base);
            emit8(encoder, 0x8bU);
        }
    }
    emit_mem_reg(encoder, base, reg, displacement);
}

static void emit_store_mem(Encoder *encoder, unsigned src, unsigned base, int64_t displacement,
                           size_t width)
{
    emit_rex(encoder, width == 8U, src, base);
    emit8(encoder, 0x89U);
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
    return encoder->builder->symbol_count - 1U;
}

static bool symbol_is_local(const Symbol *symbol)
{
    return symbol != NULL && symbol->has_frame_offset &&
           symbol->storage != STORAGE_STATIC && symbol->storage != STORAGE_EXTERN;
}

static void encode_expr(Encoder *encoder, IrInst *inst);

static void encode_call(Encoder *encoder, IrInst *inst)
{
    size_t argument_count = 0U;
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) ++argument_count;
    if (argument_count > 6U) {
        encoder_error(encoder, 4020U, &inst->location,
                      "more than six integer call arguments is deferred");
        return;
    }
    bool direct = inst->a != NULL && inst->a->op == IR_ADDR && inst->a->symbol != NULL;
    if (!direct) {
        encode_expr(encoder, inst->a);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
    }
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) {
        encode_expr(encoder, arg);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
    }
    for (size_t i = argument_count; i > 0U; --i) {
        size_t index = i - 1U;
        emit_pop(encoder, argument_registers[index]);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
    }
    if (direct) {
        emit8(encoder, 0xe8U);
        size_t field = encoder->code_size;
        emit32(encoder, 0U);
        (void)object_add_relocation(encoder->builder, encoder->text,
                                    encoder->function_offset + field,
                                    inst->a->symbol, CC64O_REL_PC32, 0, 4U);
    } else {
        emit_pop(encoder, 11U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
        emit8(encoder, 0xffU);
        emit_modrm_reg(encoder, 2U, 3U);
    }
    if ((encoder->stack_depth & 1U) != 0U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 8U);
    }
    /* The call displacement is patched by the linker. */
    if (direct) {
        /* Nothing to do: the relocation was recorded above. */
    }
    if ((encoder->stack_depth & 1U) != 0U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 8U);
    }
    if (inst->type != NULL && type_size(inst->type) == 4U) {
        emit8(encoder, 0x89U); emit_modrm_reg(encoder, 0U, 0U);
    }
}

static void encode_load(Encoder *encoder, IrInst *inst)
{
    size_t width = inst->width == 0U ? 8U : inst->width;
    if (inst->a == NULL && inst->symbol != NULL) {
        if (symbol_is_local(inst->symbol)) {
            emit_load_mem(encoder, 0U, 5U, (int64_t)inst->symbol->offset, width,
                          inst->is_signed);
        } else {
            if (width == 1U || width == 2U) {
                emit_rex(encoder, false, 0U, 0U);
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
    if (width == 1U || width == 2U) {
        emit8(encoder, inst->is_signed ? (width == 1U ? 0x0fU : 0x0fU) : 0x0fU);
        emit8(encoder, inst->is_signed ? (width == 1U ? 0xbeU : 0xbfU) : (width == 1U ? 0xb6U : 0xb7U));
        emit_modrm_reg(encoder, 0U, 0U);
    } else if (width == 4U) {
        if (inst->is_signed) {
            emit_rex(encoder, true, 0U, 0U);
            emit8(encoder, 0x63U);
        } else {
            emit_rex(encoder, false, 0U, 0U);
            emit8(encoder, 0x8bU);
        }
        emit_modrm_reg(encoder, 0U, 0U);
    } else {
        emit8(encoder, 0x48U); emit8(encoder, 0x8bU); emit_modrm_reg(encoder, 0U, 0U);
    }
}

static void encode_store(Encoder *encoder, IrInst *inst)
{
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

static void encode_binary(Encoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    switch (inst->op) {
    case IR_ADD: emit_alu(encoder, 0x01U, 0U, 1U); break;
    case IR_SUB: emit_alu(encoder, 0x29U, 0U, 1U); break;
    case IR_MUL: emit_rex(encoder, true, 1U, 0U); emit8(encoder, 0x0fU); emit8(encoder, 0xafU); emit_modrm_reg(encoder, 1U, 0U); break;
    case IR_DIV:
        if (inst->is_signed) { emit8(encoder, 0x48U); emit8(encoder, 0x99U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 7U, 1U); }
        else { emit8(encoder, 0x31U); emit_modrm_reg(encoder, 2U, 2U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 6U, 1U); }
        break;
    case IR_MOD:
        if (inst->is_signed) { emit8(encoder, 0x48U); emit8(encoder, 0x99U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 7U, 1U); emit_mov_reg_reg(encoder, 0U, 2U); }
        else { emit8(encoder, 0x31U); emit_modrm_reg(encoder, 2U, 2U); emit8(encoder, 0x48U); emit8(encoder, 0xf7U); emit_modrm_reg(encoder, 6U, 1U); emit_mov_reg_reg(encoder, 0U, 2U); }
        break;
    case IR_SHL: emit8(encoder, 0x48U); emit8(encoder, 0xd3U); emit8(encoder, 0xe0U); break;
    case IR_SHR: emit8(encoder, 0x48U); emit8(encoder, 0xd3U); emit8(encoder, inst->is_signed ? 0xf8U : 0xe8U); break;
    case IR_BIT_AND: emit_alu(encoder, 0x21U, 0U, 1U); break;
    case IR_BIT_OR: emit_alu(encoder, 0x09U, 0U, 1U); break;
    case IR_BIT_XOR: emit_alu(encoder, 0x31U, 0U, 1U); break;
    case IR_LOGICAL_AND: emit_alu(encoder, 0x21U, 0U, 1U); emit8(encoder, 0x48U); emit8(encoder, 0x85U); emit_modrm_reg(encoder, 0U, 0U); emit8(encoder, 0x0fU); emit8(encoder, 0x95U); emit8(encoder, 0xc0U); break;
    case IR_LOGICAL_OR: emit_alu(encoder, 0x09U, 0U, 1U); emit8(encoder, 0x48U); emit8(encoder, 0x85U); emit_modrm_reg(encoder, 0U, 0U); emit8(encoder, 0x0fU); emit8(encoder, 0x95U); emit8(encoder, 0xc0U); break;
    default: break;
    }
    if (inst->op == IR_LOGICAL_AND || inst->op == IR_LOGICAL_OR) {
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
    }
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
    encode_expr(encoder, inst->a);
    size_t width = inst->width == 0U ? 8U : inst->width;
    if (width == 1U) {
        if (type_is_signed(inst->type)) { emit8(encoder, 0x48U); emit8(encoder, 0x0fU); emit8(encoder, 0xbeU); emit8(encoder, 0xc0U); }
        else { emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U); }
    } else if (width == 2U) {
        if (type_is_signed(inst->type)) { emit8(encoder, 0x48U); emit8(encoder, 0x0fU); emit8(encoder, 0xbfU); emit8(encoder, 0xc0U); }
        else { emit8(encoder, 0x0fU); emit8(encoder, 0xb7U); emit8(encoder, 0xc0U); }
    } else if (width == 4U) {
        emit8(encoder, 0x89U); emit_modrm_reg(encoder, 0U, 0U);
    }
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
    case IR_CONST: emit_mov_reg_imm(encoder, 0U, inst->immediate,
                                    inst->width == 0U ? 8U : (unsigned)inst->width); break;
    case IR_FLOAT_CONST: encoder_error(encoder, 4021U, &inst->location, "floating constants are deferred"); break;
    case IR_LOAD: encode_load(encoder, inst); break;
    case IR_STORE: encode_store(encoder, inst); break;
    case IR_ADDR: encode_addr(encoder, inst); break;
    case IR_MEMBER: encode_member(encoder, inst); break;
    case IR_CALL: encode_call(encoder, inst); break;
    case IR_CAST: encode_cast(encoder, inst); break;
    case IR_COMMA: encode_comma(encoder, inst); break;
    case IR_CONDITIONAL: encode_conditional(encoder, inst); break;
    case IR_COMPARE: {
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
    case IR_LOGICAL_AND: case IR_LOGICAL_OR:
        encode_binary(encoder, inst); break;
    case IR_NEG: case IR_BIT_NOT: case IR_LOGICAL_NOT: encode_unary(encoder, inst); break;
    default: encoder_error(encoder, 4023U, &inst->location, "invalid expression IR opcode"); break;
    }
}

static void encode_return(Encoder *encoder, IrInst *inst)
{
    if (inst->a != NULL) encode_expr(encoder, inst->a);
    emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit_modrm_reg(encoder, 5U, 4U); /* mov rbp,rsp */
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
    size_t index = 0U;
    for (Symbol *parameter = function->symbol->type->parameters; parameter != NULL;
         parameter = parameter->next, ++index) {
        if (index >= 6U) break;
        unsigned reg = argument_registers[index];
        emit_store_mem(encoder, reg, 5U, (int64_t)parameter->offset, 8U);
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

static bool append_global_data(Encoder *encoder, IrProgram *program)
{
    for (size_t i = 0U; i < program->global_count; ++i) {
        Symbol *symbol = program->global_symbols[i];
        if (symbol == NULL || symbol->initializer == NULL) continue;
        size_t size = type_size(symbol->type);
        if (size == 0U) size = 1U;
        unsigned char *bytes = calloc(size, 1U);
        if (bytes == NULL) return false;
        AstNode *initializer = symbol->initializer;
        AstNode *value = initializer->kind == NODE_INITIALIZER ? initializer->a : initializer;
        while (value != NULL && value->kind == NODE_CAST) value = value->a;
        if (value != NULL && value->kind == NODE_INTEGER) {
            uint64_t raw = value->unsigned_integer;
            for (size_t byte = 0U; byte < size && byte < 8U; ++byte) bytes[byte] = (unsigned char)(raw >> (byte * 8U));
        } else if (value != NULL && value->kind == NODE_STRING) {
            size_t copy = value->text_length < size ? value->text_length : size;
            if (copy != 0U) memcpy(bytes, value->text, copy);
        } else {
            free(bytes);
            encoder_error(encoder, 4030U, NULL, "global initializer is not representable yet");
            return false;
        }
        size_t offset = encoder->data->size;
        if (!object_append_data(encoder->builder, encoder->data, bytes, size)) { free(bytes); return false; }
        free(bytes);
        for (size_t j = 0U; j < encoder->builder->symbol_count; ++j) {
            if (strcmp(encoder->builder->symbols[j].name, symbol->name) == 0) {
                encoder->builder->symbols[j].value = offset;
                encoder->builder->symbols[j].size = size;
                break;
            }
        }
    }
    return true;
}

static bool encode_function(Encoder *encoder, IrFunction *function)
{
    encoder->code = NULL; encoder->code_size = 0U; encoder->code_capacity = 0U;
    encoder->stack_depth = 0U;
    encoder->label_count = 0U; encoder->fixup_count = 0U;
    encoder->function_offset = encoder->text->size;
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
    if (!prepare_symbols(&encoder, program) || !append_global_data(&encoder, program)) return false;
    for (IrFunction *function = program->functions; function != NULL; function = function->next) {
        if (!encode_function(&encoder, function)) return false;
    }
    free(encoder.labels);
    free(encoder.fixups);
    free(encoder.code);
    return !encoder.failed;
}
