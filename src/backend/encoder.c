#include "backend/encoder.h"

#include "backend/abi.h"

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

struct IrEncoder {
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
    const IrFunction *function;
    Symbol *main_symbol;
    size_t stack_depth;
    LabelSlot *labels;
    size_t label_count;
    size_t label_capacity;
    LabelFixup *fixups;
    size_t fixup_count;
    size_t fixup_capacity;
    bool failed;
};

static const unsigned char argument_registers[6] = {7U, 6U, 2U, 1U, 8U, 9U};

static void encoder_error(IrEncoder *encoder, unsigned id, const SourceLocation *location,
                          const char *message)
{
    diagnostic_emit(encoder->diagnostics, id, DIAG_BACKEND,
                    location == NULL ? NULL : location->source,
                    location == NULL ? 0U : location->line,
                    location == NULL ? 0U : location->column, message);
    encoder->failed = true;
}

static bool ensure_code(IrEncoder *encoder, size_t extra)
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

static void emit8(IrEncoder *encoder, unsigned char value)
{
    if (!ensure_code(encoder, 1U)) { encoder->failed = true; return; }
    encoder->code[encoder->code_size++] = value;
}

static void emit32(IrEncoder *encoder, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; ++i) emit8(encoder, (unsigned char)(value >> (i * 8U)));
}

static void emit64(IrEncoder *encoder, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) emit8(encoder, (unsigned char)(value >> (i * 8U)));
}

static void emit_rex(IrEncoder *encoder, bool w, unsigned reg, unsigned rm)
{
    unsigned value = (w ? 8U : 0U) | ((reg >= 8U) ? 4U : 0U) | ((rm >= 8U) ? 1U : 0U);
    if (value != 0U) emit8(encoder, (unsigned char)(0x40U | value));
}

/* An eight- or sixteen-bit register operand names one of the low four
   registers only when a REX prefix is present: without one, numbers four
   through seven name the legacy high byte and word registers instead. The
   prefix is written whenever an operand could land in that range, carrying the
   extension bits it would otherwise carry, and is left off when neither
   operand can need it, because an unnecessary prefix is not free. */
static void emit_rex_byte_word(IrEncoder *encoder, unsigned reg, unsigned rm)
{
    unsigned value = ((reg >= 8U) ? 4U : 0U) | ((rm >= 8U) ? 1U : 0U);
    if (value != 0U || reg >= 4U || rm >= 4U) {
        emit8(encoder, (unsigned char)(0x40U | value));
    }
}

static void emit_modrm_reg(IrEncoder *encoder, unsigned reg, unsigned rm)
{
    emit8(encoder, (unsigned char)(0xc0U | ((reg & 7U) << 3) | (rm & 7U)));
}

static void emit_mem_reg(IrEncoder *encoder, unsigned base, unsigned reg,
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

static void emit_mem_rip(IrEncoder *encoder, unsigned reg)
{
    emit8(encoder, (unsigned char)(((reg & 7U) << 3) | 5U));
}

/* LEA reg, [base + displacement]. emit_mem_reg only writes the addressing
   bytes, so the REX prefix and opcode are emitted here. */
static void emit_lea_mem(IrEncoder *encoder, unsigned base, unsigned reg,
                         int64_t displacement)
{
    emit_rex(encoder, true, reg, base);
    emit8(encoder, 0x8dU);
    emit_mem_reg(encoder, base, reg, displacement);
}

static void emit_mov_reg_reg(IrEncoder *encoder, unsigned dst, unsigned src)
{
    emit_rex(encoder, true, src, dst);
    emit8(encoder, 0x89U);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_mov_reg32_reg(IrEncoder *encoder, unsigned dst, unsigned src)
{
    if (src >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, 0x89U);
    emit_modrm_reg(encoder, src, dst);
}

/* Opcode 88 is "MOV r/m8, r8": the destination is the r/m operand and the
   source is the reg operand, so the register arguments are passed in the
   opposite order from the 64-bit move. */
static void emit_mov_reg8_reg(IrEncoder *encoder, unsigned dst, unsigned src)
{
    emit_rex_byte_word(encoder, src, dst);
    emit8(encoder, 0x88U);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_mov_reg_imm(IrEncoder *encoder, unsigned reg, uint64_t value, unsigned width)
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

static void emit_push(IrEncoder *encoder, unsigned reg)
{
    if (reg >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, (unsigned char)(0x50U + (reg & 7U)));
}

static void emit_pop(IrEncoder *encoder, unsigned reg)
{
    if (reg >= 8U) emit8(encoder, 0x41U);
    emit8(encoder, (unsigned char)(0x58U + (reg & 7U)));
}

static void emit_alu(IrEncoder *encoder, unsigned opcode, unsigned dst, unsigned src)
{
    emit_rex(encoder, true, src, dst);
    emit8(encoder, (unsigned char)opcode);
    emit_modrm_reg(encoder, src, dst);
}

static void emit_test(IrEncoder *encoder, unsigned reg)
{
    emit_rex(encoder, true, reg, reg);
    emit8(encoder, 0x85U);
    emit_modrm_reg(encoder, reg, reg);
}

static void emit_setcc(IrEncoder *encoder, unsigned condition)
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
    /* The comparison always computes a - b, so the greater forms need the
       "above" conditions and the less forms the "below" ones. */
    case COMPARE_GREATER: *code = is_signed ? 15U : 7U; return true;
    case COMPARE_GREATER_EQUAL: *code = is_signed ? 13U : 3U; return true;
    }
    *code = 0U;
    return false;
}

static void emit_load_mem(IrEncoder *encoder, unsigned reg, unsigned base, int64_t displacement,
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

static void emit_store_mem(IrEncoder *encoder, unsigned src, unsigned base, int64_t displacement,
                           size_t width)
{
    if (width == 2U) emit8(encoder, 0x66U);
    if (width == 1U || width == 2U) {
        emit_rex_byte_word(encoder, src, base);
    } else {
        emit_rex(encoder, width == 8U, src, base);
    }
    emit8(encoder, width == 1U ? 0x88U : 0x89U);
    emit_mem_reg(encoder, base, src, displacement);
}

/* An eightbyte at the end of an object can be shorter than eight bytes, so a
   move of it has to be no wider than what the object has room for. The count
   is taken apart into the widest chunk that still fits, which reaches any
   length from one to eight without reading or writing past the object. */
static size_t next_chunk(size_t *count)
{
    if (*count >= 8U) { *count -= 8U; return 8U; }
    if (*count >= 4U) { *count -= 4U; return 4U; }
    if (*count >= 2U) { *count -= 2U; return 2U; }
    *count = 0U;
    return 1U;
}

static void emit_load_partial(IrEncoder *encoder, unsigned reg, unsigned base,
                              int64_t displacement, size_t count)
{
    while (count != 0U) {
        size_t width = next_chunk(&count);
        emit_load_mem(encoder, reg, base, displacement, width, false);
        displacement += (int64_t)width;
    }
}

static void emit_store_partial(IrEncoder *encoder, unsigned reg, unsigned base,
                               int64_t displacement, size_t count)
{
    while (count != 0U) {
        size_t width = next_chunk(&count);
        emit_store_mem(encoder, reg, base, displacement, width);
        displacement += (int64_t)width;
    }
}

/* Move one eightbyte of an aggregate between memory and a vector register. A
   piece whose class is the vector one holds a float or a double and nothing
   else, so it is either four or eight bytes wide and the two moves below cover
   both. */
/* Move one eightbyte of an aggregate between memory and a vector register. A
   piece whose class is the vector one holds a float or a double and nothing
   else, so it is either four or eight bytes wide and the two moves below cover
   both.

   The two share one REX prefix: its B bit extends the memory operand's base
   register and its R bit extends the vector register. A base register above
   R7 therefore shifts the vector register as well, so a load from such a base
   reads the eightbyte into a general register first and moves it across, which
   costs one instruction and cannot address the wrong register. */
static void emit_load_piece_vector(IrEncoder *encoder, unsigned index,
                                   unsigned base, int64_t displacement,
                                   size_t width)
{
    if (base >= 8U) {
        emit_load_partial(encoder, 10U, base, displacement, width);
        emit8(encoder, 0x66U);
        /* The REX prefix follows the legacy prefix and precedes the opcode. */
        emit_rex(encoder, width == 8U, index, 10U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x6eU);
        emit_modrm_reg(encoder, index, 10U);
        return;
    }
    emit8(encoder, width == 4U ? 0xf3U : 0xf2U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x10U);
    emit_rex(encoder, false, index, base);
    emit_mem_reg(encoder, base, index, displacement);
}

static void emit_store_piece_vector(IrEncoder *encoder, unsigned index,
                                    unsigned base, int64_t displacement,
                                    size_t width)
{
    emit8(encoder, width == 4U ? 0xf3U : 0xf2U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
    emit_rex(encoder, false, index, base);
    emit_mem_reg(encoder, base, index, displacement);
}

static size_t add_label(IrEncoder *encoder, size_t id)
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

static void add_fixup(IrEncoder *encoder, size_t field, size_t label)
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

static void emit_jump(IrEncoder *encoder, size_t label)
{
    emit8(encoder, 0xe9U);
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    add_fixup(encoder, field, label);
}

static void emit_conditional_jump(IrEncoder *encoder, unsigned condition, size_t label)
{
    emit8(encoder, 0x0fU);
    emit8(encoder, (unsigned char)(0x80U + condition));
    size_t field = encoder->code_size;
    emit32(encoder, 0U);
    add_fixup(encoder, field, label);
}

static bool resolve_labels(IrEncoder *encoder)
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

static void define_label(IrEncoder *encoder, size_t id)
{
    size_t index = add_label(encoder, id);
    encoder->labels[index].offset = encoder->code_size;
    encoder->labels[index].defined = true;
}

static void emit_rip_reference(IrEncoder *encoder, unsigned opcode, unsigned reg, Symbol *symbol)
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

static size_t object_symbol_index(IrEncoder *encoder, Symbol *symbol)
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

static void encode_expr(IrEncoder *encoder, IrInst *inst);
static bool is_float_type(const Type *type);
static void emit_normalize(IrEncoder *encoder, size_t width, bool is_signed);
static void emit_load_rax(IrEncoder *encoder, size_t width, bool is_signed);

static void emit_save_xmm0(IrEncoder *encoder, size_t width)
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

static void emit_restore_xmm(IrEncoder *encoder, unsigned index, size_t width)
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

/* A value carried as a sequence of eightbytes rather than in one register:
   a record, or the 128-bit integer, which is two general registers wide. */
static bool argument_is_aggregate(const Type *type)
{
    return type != NULL && (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION ||
                            type->kind == TYPE_UINT128);
}

/* The `index`th argument of a call, counting from zero. The list is walked
   because the encoder has no room for an index per argument. */
static IrInst *nth_argument(const IrInst *inst, size_t index)
{
    IrInst *arg = inst->args;
    for (size_t i = 0U; i < index && arg != NULL; ++i) arg = arg->next;
    return arg;
}

/* Fill one slot per argument of a call. The same routine serves the caller,
   which knows the types of the expressions it is passing, and the callee, which
   knows the types it declared: both must reach the same answer, so both ask
   the same question here. The types are returned because the caller needs the
   size of each argument as well as where it goes. */
static size_t call_slots(IrEncoder *encoder, const IrInst *inst, ArgSlot **result,
                         Type ***types_result)
{
    size_t count = 0U;
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) ++count;
    *result = NULL;
    *types_result = NULL;
    if (count == 0U) return 0U;
    ArgSlot *slots = cc64_xcalloc(count, sizeof(*slots));
    Type **types = cc64_xcalloc(count, sizeof(*types));
    if (slots == NULL || types == NULL) {
        free(slots);
        free(types);
        encoder_error(encoder, 4020U, &inst->location, "cannot record call arguments");
        return 0U;
    }
    size_t index = 0U;
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next, ++index) {
        types[index] = arg->type;
    }
    AggClass return_pieces[2];
    bool returns_memory = false;
    if (inst->type != NULL && argument_is_aggregate(inst->type)) {
        (void)abi_aggregate_pieces(inst->type, return_pieces, &returns_memory);
    }
    size_t area = abi_assign_arguments(types, index, returns_memory, slots);
    *result = slots;
    *types_result = types;
    return area;
}

/* Push one aggregate onto the outgoing argument area, a slot at a time, from
   the address the argument expression produced. The slots go on in reverse, so
   the object's first bytes end up at the lowest address, where the callee
   reads them. The address is copied aside first, because the slots already
   pushed would otherwise land on top of it. The last slot is no wider than
   the object, so the push never reads bytes the object does not have. */
static void emit_push_aggregate(IrEncoder *encoder, IrInst *arg, size_t *depth)
{
    encode_expr(encoder, arg);
    size_t total = type_size(arg->type);
    total = (total + 7U) & ~(size_t)7U;
    if (total <= 8U) {
        emit_push(encoder, 0U);
        ++*depth;
        return;
    }
    emit_mov_reg_reg(encoder, 10U, 0U);
    size_t size = type_size(arg->type);
    for (size_t end = total; end > 0U;) {
        size_t width = size % 8U;
        if (width == 0U || width > end) width = end >= 8U ? 8U : end;
        end -= width;
        emit_load_partial(encoder, 0U, 10U, (int64_t)end, width);
        emit_push(encoder, 0U);
        ++*depth;
    }
}

/* Read the eightbytes an argument occupies out of the address the caller left
   on top of the stack and into the registers the slot names. The address is
   read, not popped, because the pieces have to be gathered from it before the
   slot is released. */
static void emit_take_aggregate(IrEncoder *encoder, const ArgSlot *slot,
                                const Type *type)
{
    size_t remaining = type_size(type);
    emit8(encoder, 0x4cU); emit8(encoder, 0x8bU); emit8(encoder, 0x1cU);
    emit8(encoder, 0x24U);
    for (unsigned piece = 0U; piece < slot->pieces; ++piece) {
        size_t width = remaining >= 8U ? 8U : remaining;
        if (slot->classes[piece] == AGG_SSE) {
            emit_load_piece_vector(encoder, slot->registers[piece], 11U,
                                   (int64_t)piece * 8, width);
        } else {
            emit_load_partial(encoder, argument_registers[slot->registers[piece]],
                              11U, (int64_t)piece * 8, width);
        }
        remaining -= width;
    }
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 0x08U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
}

static void encode_call(IrEncoder *encoder, IrInst *inst)
{
    size_t argument_count = 0U;
    for (IrInst *arg = inst->args; arg != NULL; arg = arg->next) ++argument_count;
    if (argument_count > 64U) {
        encoder_error(encoder, 4020U, &inst->location,
                      "call argument limit exceeded");
        return;
    }
    ArgSlot *slots = NULL;
    Type **types = NULL;
    size_t area = call_slots(encoder, inst, &slots, &types);
    size_t stack_units = 0U;
    for (size_t i = 0U; i < argument_count; ++i) {
        if (!slots[i].ok) {
            encoder_error(encoder, 4023U, &inst->location,
                          "argument cannot be passed under this ABI");
            free(slots);
            free(types);
            return;
        }
        if (!slots[i].in_register) stack_units += slots[i].size / 8U;
    }
    bool direct = inst->a != NULL && inst->a->op == IR_ADDR && inst->a->symbol != NULL;
    /* __cc64_va_start is a compiler builtin: it yields the first unnamed
       integer argument slot of the enclosing variadic function. */
    if (direct && argument_count == 0U && inst->a->symbol != NULL &&
        inst->a->symbol->name != NULL &&
        strcmp(inst->a->symbol->name, "__cc64_va_start") == 0) {
        const IrFunction *current = encoder->function;
        if (current == NULL || !current->symbol->type->variadic ||
            current->va_area_offset == 0) {
            encoder_error(encoder, 4021U, &inst->location,
                          "va_start used outside a variadic function");
            free(slots);
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
        free(slots);
        free(types);
        return;
    }
    /* The stack has to be aligned before the call, and pushing the outgoing
       area changes how much is on it, so the padding is decided from the final
       size of that area rather than from the number of arguments. */
    bool adjust = ((encoder->stack_depth + stack_units) & 1U) != 0U;
    if (adjust) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 8U);
        ++encoder->stack_depth;
    }
    /* The outgoing area is filled from its high end down, so the last stack
       slot is pushed first and the first ends up at the lowest address, which
       is where a callee looks for it. */
    for (size_t index = argument_count; index > 0U; --index) {
        if (slots[index - 1U].in_register) continue;
        IrInst *arg = nth_argument(inst, index - 1U);
        if (arg == NULL) continue;
        if (argument_is_aggregate(types[index - 1U])) {
            emit_push_aggregate(encoder, arg, &encoder->stack_depth);
            continue;
        }
        encode_expr(encoder, arg);
        if (slots[index - 1U].classes[0] == AGG_SSE) {
            emit_save_xmm0(encoder, type_size(arg->type));
        } else {
            emit_push(encoder, 0U);
            ++encoder->stack_depth;
        }
    }
    /* Register arguments are evaluated in order and each is left on the stack,
       because loading one register can be undone by evaluating the next. The
       load-back pass then walks them in reverse. */
    for (size_t index = 0U; index < argument_count; ++index) {
        if (!slots[index].in_register) continue;
        IrInst *arg = nth_argument(inst, index);
        if (arg == NULL) continue;
        encode_expr(encoder, arg);
        if (argument_is_aggregate(types[index]) ||
            slots[index].classes[0] != AGG_SSE) {
            /* An aggregate is left as its address; the load-back pass reads the
               eightbytes out of the object it points at. */
            emit_push(encoder, 0U);
            ++encoder->stack_depth;
        } else {
            emit_save_xmm0(encoder, type_size(arg->type));
        }
    }
    if (!direct) {
        encode_expr(encoder, inst->a);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
        emit_pop(encoder, 11U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
    }
    for (size_t index = argument_count; index > 0U; --index) {
        size_t position = index - 1U;
        if (!slots[position].in_register) continue;
        IrInst *arg = nth_argument(inst, position);
        if (arg == NULL) continue;
        if (argument_is_aggregate(types[position])) {
            emit_take_aggregate(encoder, &slots[position], types[position]);
        } else if (slots[position].classes[0] == AGG_SSE) {
            emit_restore_xmm(encoder, slots[position].registers[0],
                             type_size(arg->type));
        } else {
            emit_pop(encoder, argument_registers[slots[position].registers[0]]);
            if (encoder->stack_depth != 0U) --encoder->stack_depth;
        }
    }
    /* A result too large for the return registers is written through a
       pointer the caller supplies, in the first general register. No named
       argument was given that register, so naming it here cannot displace one.
       It is named after the load-back, which is what fills the rest. */
    if (inst->type != NULL && argument_is_aggregate(inst->type)) {
        AggClass probe[2];
        bool memory = false;
        if (abi_aggregate_pieces(inst->type, probe, &memory) == 0U && memory) {
            emit_lea_mem(encoder, 5U, argument_registers[0], inst->offset);
        }
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
    if (area != 0U) {
        if (area <= 127U) {
            emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U);
            emit8(encoder, (unsigned char)area);
        } else {
            emit8(encoder, 0x48U); emit8(encoder, 0x81U); emit8(encoder, 0xc4U);
            emit32(encoder, (uint32_t)area);
        }
        encoder->stack_depth -= stack_units;
    }
    if (adjust) {
        emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U); emit8(encoder, 8U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
    }
    if (inst->type != NULL && argument_is_aggregate(inst->type)) {
        /* A result that came back in registers is written to the space the
           lowering reserved, and the value of the call is that address. */
        AggClass pieces[2];
        bool memory = false;
        size_t count = abi_aggregate_pieces(inst->type, pieces, &memory);
        if (count == 0U && !memory) {
            encoder_error(encoder, 4023U, &inst->location,
                          "return value cannot be passed under this ABI");
            free(slots);
            free(types);
            return;
        }
        if (memory) {
            /* The result was written through the pointer the call was given,
               and that pointer came back, so the value of the call is the
               address of the space the lowering reserved. */
            free(slots);
            free(types);
            return;
        }
        static const unsigned char return_general[2] = {0U, 2U};
        size_t remaining = type_size(inst->type);
        for (size_t piece = 0U; piece < count; ++piece) {
            size_t width = remaining >= 8U ? 8U : remaining;
            if (pieces[piece] == AGG_SSE) {
                emit_store_piece_vector(encoder, (unsigned)piece, 5U,
                                        inst->offset + (int64_t)piece * 8, width);
            } else {
                emit_store_partial(encoder, return_general[piece], 5U,
                                   inst->offset + (int64_t)piece * 8, width);
            }
            remaining -= width;
        }
        emit_rex(encoder, true, 0U, 5U);
        emit8(encoder, 0x8dU);
        emit_mem_reg(encoder, 5U, 0U, inst->offset);
        free(slots);
        free(types);
        return;
    }
    if (inst->type != NULL && !is_float_type(inst->type)) {
        emit_normalize(encoder, type_size(inst->type), type_is_signed(inst->type));
    }
    free(slots);
    free(types);
}

static bool is_float_type(const Type *type)
{
    return type != NULL && (type->kind == TYPE_FLOAT || type->kind == TYPE_DOUBLE);
}

static void emit_xmm_load_rax(IrEncoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U); emit8(encoder, 0x00U);
    } else {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x10U); emit8(encoder, 0x00U);
    }
}

static void emit_xmm_move_from_rax(IrEncoder *encoder)
{
    emit8(encoder, 0x66U); emit8(encoder, 0x48U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x6eU);
    emit8(encoder, 0xc0U);
}

static void emit_xmm_move_to_rax(IrEncoder *encoder, size_t width)
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

static void emit_xmm_from_stack(IrEncoder *encoder)
{
    emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x10U); emit8(encoder, 0x04U);
    emit8(encoder, 0x24U);
}

static void emit_xmm_to_stack(IrEncoder *encoder)
{
    emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x11U); emit8(encoder, 0x04U);
    emit8(encoder, 0x24U);
}

static void encode_float_constant(IrEncoder *encoder, IrInst *inst)
{
    if (inst->type != NULL && inst->type->kind == TYPE_FLOAT) {
        float value = (float)inst->value.floating;
        uint32_t bits = 0U;
        memcpy(&bits, &value, sizeof(bits));
        emit_mov_reg_imm(encoder, 0U, bits, 8U);
    } else {
        uint64_t bits = 0U;
        double value = inst->value.floating;
        memcpy(&bits, &value, sizeof(bits));
        emit_mov_reg_imm(encoder, 0U, bits, 8U);
    }
    emit_xmm_move_from_rax(encoder);
}

static void encode_load(IrEncoder *encoder, IrInst *inst)
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
                          (((inst->flags) & IR_FLAG_SIGNED) != 0U));
        } else {
            if (width == 1U || width == 2U) {
                emit_rex(encoder, true, 0U, 0U);
                emit8(encoder, 0x0fU);
                emit8(encoder, (((inst->flags) & IR_FLAG_SIGNED) != 0U) ? (width == 1U ? 0xbeU : 0xbfU) :
                              (width == 1U ? 0xb6U : 0xb7U));
            } else if (width == 4U) {
                if ((((inst->flags) & IR_FLAG_SIGNED) != 0U)) {
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
    emit_load_rax(encoder, width, (((inst->flags) & IR_FLAG_SIGNED) != 0U));
}

static void encode_store(IrEncoder *encoder, IrInst *inst)
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

static void encode_addr(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a == NULL) {
        if (inst->symbol != NULL) {
            if (symbol_is_local(inst->symbol)) {
                emit_rex(encoder, true, 0U, 5U);
                emit8(encoder, 0x8dU);
                emit_mem_reg(encoder, 5U, 0U, (int64_t)inst->symbol->offset);
            } else {
                emit_rip_reference(encoder, 0x8dU, 0U, inst->symbol);
            }
            return;
        }
        /* No name and nothing to compute: the address is a frame offset the
           lowering chose, which is how the storage of a compound literal is
           named. */
        emit_rex(encoder, true, 0U, 5U);
        emit8(encoder, 0x8dU);
        emit_mem_reg(encoder, 5U, 0U, inst->offset);
        return;
    }
    encode_expr(encoder, inst->a);
}

static void encode_member(IrEncoder *encoder, IrInst *inst)
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

static void emit_binary_opcode(IrEncoder *encoder, IrOp op, bool is_signed)
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

/* There is no instruction that negates a floating value, so the sign bit is
   flipped against a mask. Doing it this way rather than by subtracting from
   zero keeps a negative zero negative, which a program's division can observe
   and which the definition of negation requires. */
static void emit_xmm_negate(IrEncoder *encoder, size_t width)
{
    /* The mask is moved into the second vector register with the sixty-four-bit
       form for both widths. The thirty-two-bit form of that move is spelled
       without a width bit, and on this target it is decoded as a move to a
       multimedia register rather than to a vector one, so the mask would land
       where the exclusive-or cannot see it and the negation would do nothing.
       Loading the thirty-two-bit mask through the wider move keeps the
       upper bits zero, which the exclusive-or of the low half needs not care
       about. */
    emit_mov_reg_imm(encoder, 0U,
                     width == 4U ? 0x80000000UL : 0x8000000000000000UL, 8U);
    emit8(encoder, 0x66U); emit_rex(encoder, true, 0U, 0U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x6eU);
    emit_modrm_reg(encoder, 1U, 0U);              /* movq xmm1, rax */
    if (width == 4U) {
        emit8(encoder, 0x0fU); emit8(encoder, 0x57U);
        emit8(encoder, 0xc1U);                    /* xorps xmm0, xmm1 */
        return;
    }
    emit8(encoder, 0x66U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x57U); emit8(encoder, 0xc1U); /* xorpd xmm0, xmm1 */
}

/* The step of one, or of minus one, in the second vector register. It is the
   sixty-four-bit form in both directions because a single-precision step would
   be taken apart and put back together for no gain, and because the narrow move
   form is decoded differently on this target. */
static void emit_xmm_step(IrEncoder *encoder, bool down)
{
    emit_mov_reg_imm(encoder, 0U,
                     down ? 0xBFF0000000000000UL : 0x3FF0000000000000UL, 8U);
    emit8(encoder, 0x66U); emit_rex(encoder, true, 0U, 0U);
    emit8(encoder, 0x0fU); emit8(encoder, 0x6eU);
    emit_modrm_reg(encoder, 1U, 0U);              /* movq xmm1, rax */
}

/* A floating value is computed into the first vector register, so the second
   operand of a two-operand instruction is moved from there into the second
   register. The move is a vector-to-vector one: the form that reads a general
   register would take whatever bits that register happened to hold, which for
   a value loaded from memory is nothing in particular. */
static void emit_xmm_second_operand(IrEncoder *encoder)
{
    emit8(encoder, 0x66U); emit8(encoder, 0x0fU);
    emit8(encoder, 0x28U); emit8(encoder, 0xc8U);   /* movapd xmm1, xmm0 */
}

static void emit_xmm_convert_to_double(IrEncoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x5aU); emit8(encoder, 0xc0U);
    }
}

static void emit_xmm_convert_from_double(IrEncoder *encoder, size_t width)
{
    if (width == 4U) {
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x5aU); emit8(encoder, 0xc0U);
    }
}

static void encode_float_binary(IrEncoder *encoder, IrInst *inst)
{
    size_t width = inst->type == NULL ? 8U : type_size(inst->type);
    encode_expr(encoder, inst->a);
    emit_xmm_convert_to_double(encoder, inst->a == NULL ? 8U : type_size(inst->a->type));
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    emit_xmm_to_stack(encoder);
    encode_expr(encoder, inst->b);
    emit_xmm_convert_to_double(encoder, inst->b == NULL ? 8U : type_size(inst->b->type));
    emit_xmm_second_operand(encoder);
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

static void encode_float_compare(IrEncoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_xmm_convert_to_double(encoder, inst->a == NULL ? 8U : type_size(inst->a->type));
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU); emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    emit_xmm_to_stack(encoder);
    encode_expr(encoder, inst->b);
    emit_xmm_convert_to_double(encoder, inst->b == NULL ? 8U : type_size(inst->b->type));
    emit_xmm_second_operand(encoder);
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

static void encode_binary(IrEncoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_binary_opcode(encoder, inst->op, (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_normalize(encoder, inst->width == 0U ? 8U : inst->width,
                   (((inst->flags) & IR_FLAG_SIGNED) != 0U));
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

/* A compound assignment on a floating object: the stored value and the
   right-hand side are both widened to the same working form, combined there,
   and narrowed back before the result goes into the object. The address is
   kept across both operands because the right-hand side may itself write
   memory. */
static void encode_compound_float(IrEncoder *encoder, IrInst *inst)
{
    size_t width = inst->width == 0U ? 8U : inst->width;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_xmm_load_rax(encoder, width);
    emit_xmm_convert_to_double(encoder, width);
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xecU);
    emit8(encoder, 0x08U);
    ++encoder->stack_depth;
    emit_xmm_to_stack(encoder);
    encode_expr(encoder, inst->b);
    emit_xmm_convert_to_double(encoder, inst->b == NULL ? width
                                                       : type_size(inst->b->type));
    emit_xmm_second_operand(encoder);
    emit_xmm_from_stack(encoder);
    emit8(encoder, 0x48U); emit8(encoder, 0x83U); emit8(encoder, 0xc4U);
    emit8(encoder, 0x08U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    switch (compound_ir_op(inst->binary)) {
    case IR_ADD: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x58U); emit8(encoder, 0xc1U); break;
    case IR_SUB: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x5cU); emit8(encoder, 0xc1U); break;
    case IR_MUL: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x59U); emit8(encoder, 0xc1U); break;
    case IR_DIV: emit8(encoder, 0xf2U); emit8(encoder, 0x0fU); emit8(encoder, 0x5eU); emit8(encoder, 0xc1U); break;
    default:
        encoder_error(encoder, 4022U, &inst->location,
                      "operator cannot be applied to a floating object");
        return;
    }
    emit_xmm_convert_from_double(encoder, width);
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    if (width == 4U) {
        emit8(encoder, 0xf3U); emit_rex(encoder, false, 0U, 11U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
        emit_mem_reg(encoder, 11U, 0U, 0);
    } else {
        emit8(encoder, 0xf2U); emit_rex(encoder, false, 0U, 11U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
        emit_mem_reg(encoder, 11U, 0U, 0);
    }
}

static void encode_compound(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a == NULL || inst->b == NULL) return;
    if (is_float_type(inst->type)) {
        encode_compound_float(encoder, inst);
        return;
    }
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_load_rax(encoder, inst->width == 0U ? 8U : inst->width, (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 1U, 0U);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_binary_opcode(encoder, compound_ir_op(inst->binary), (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_normalize(encoder, inst->width == 0U ? 8U : inst->width,
                   (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_store_mem(encoder, 0U, 11U, 0,
                   inst->width == 0U ? 8U : inst->width);
}

static void emit_normalize(IrEncoder *encoder, size_t width, bool is_signed)
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

static void emit_load_rax(IrEncoder *encoder, size_t width, bool is_signed)
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

static void emit_add_imm(IrEncoder *encoder, unsigned reg, int64_t value,
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

static void encode_logical(IrEncoder *encoder, IrInst *inst)
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

/* `rep movsb` leaves the destination register one object past the last byte it
   wrote, so the address of the object has to be kept elsewhere. R11 is chosen
   because the copy uses the three registers the instruction itself names and
   because no caller of a copy has anything live in it. */
static void emit_object_copy(IrEncoder *encoder, size_t size)
{
    emit_mov_reg_imm(encoder, 1U, size, 8U);
    emit8(encoder, 0xf3U); emit8(encoder, 0xa4U);
}

static void encode_copy(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a == NULL || inst->b == NULL) return;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_mov_reg_reg(encoder, 6U, 0U);
    emit_pop(encoder, 7U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_mov_reg_reg(encoder, 11U, 7U);
    emit_object_copy(encoder, inst->value.immediate);
    emit_mov_reg_reg(encoder, 0U, 11U);
}

static void encode_zero(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a == NULL) return;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_mov_reg_reg(encoder, 7U, 0U);
    emit_mov_reg_imm(encoder, 1U, inst->value.immediate, 8U);
    emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    emit8(encoder, 0xf3U); emit8(encoder, 0xaaU);
    emit_pop(encoder, 0U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
}

static void encode_increment(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a == NULL) {
        encoder_error(encoder, 4024U, &inst->location,
                      "increment requires an address");
        return;
    }
    size_t width = inst->width == 0U ? 8U : inst->width;
    if (is_float_type(inst->type)) {
        /* A floating object moves through a vector register, so the address
           travels on the stack while the value, the old value, and the step
           occupy vector registers of their own. The value before the step is
           kept in a register rather than on the stack because the address is
           already there, and a second stack slot would be the same one. */
        size_t operand = inst->type == NULL ? 8U : type_size(inst->type);
        bool post = ((inst->flags) & IR_FLAG_POST) != 0;
        /* The step is a signed count; a decrement is recorded as a negative
           one, and the immediate field is unsigned, so the sign is read back
           rather than compared against zero. */
        bool down = (int64_t)inst->value.immediate < 0;
        encode_expr(encoder, inst->a);
        emit_push(encoder, 0U);
        ++encoder->stack_depth;
        emit_xmm_load_rax(encoder, operand);
        emit_xmm_convert_to_double(encoder, operand);
        emit8(encoder, 0x66U); emit_rex(encoder, true, 0U, 0U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x28U);
        emit_modrm_reg(encoder, 3U, 0U);          /* movapd xmm3, xmm0 */
        emit_xmm_step(encoder, down);
        emit8(encoder, 0xf2U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x58U); emit8(encoder, 0xc1U);   /* addsd xmm0, xmm1 */
        emit_xmm_convert_from_double(encoder, operand);
        emit_pop(encoder, 11U);
        if (encoder->stack_depth != 0U) --encoder->stack_depth;
        if (operand == 4U) {
            emit8(encoder, 0xf3U); emit_rex(encoder, false, 0U, 11U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
        } else {
            emit8(encoder, 0xf2U); emit_rex(encoder, false, 0U, 11U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x11U);
        }
        emit_mem_reg(encoder, 11U, 0U, 0);
        /* A post-increment's value is the one before the step and a
           pre-increment's is the one after. */
        if (post) {
            emit8(encoder, 0x66U); emit_rex(encoder, true, 3U, 0U);
            emit8(encoder, 0x0fU); emit8(encoder, 0x28U);
            emit_modrm_reg(encoder, 0U, 3U);      /* movapd xmm0, xmm3 */
        }
        return;
    }
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    emit_load_rax(encoder, width, (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_mov_reg_reg(encoder, 10U, 0U);
    emit_add_imm(encoder, 0U, (int64_t)inst->value.immediate, width);
    emit_normalize(encoder, width, (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_store_mem(encoder, 0U, 11U, 0, width);
    if ((((inst->flags) & IR_FLAG_POST) != 0U)) emit_mov_reg_reg(encoder, 0U, 10U);
}

static void encode_switch(IrEncoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    if (inst->a != NULL && inst->a->width == 4U) {
        if ((inst->a->flags & IR_FLAG_SIGNED) != 0U) {
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

static void encode_unary(IrEncoder *encoder, IrInst *inst)
{
    /* Which form an operator takes is decided by the type of its operand, not
       by the type of its result: the result of a logical negation is an integer
       whatever the operand was, and testing the result's type would send a
       floating operand down the integer path. */
    bool float_operand = inst->a != NULL && is_float_type(inst->a->type);
    if ((is_float_type(inst->type) || float_operand) &&
        (inst->op == IR_NEG || inst->op == IR_LOGICAL_NOT)) {
        size_t width = (inst->type != NULL && is_float_type(inst->type))
                           ? type_size(inst->type)
                           : (inst->a == NULL ? 8U : type_size(inst->a->type));
        encode_expr(encoder, inst->a);
        /* A negation leaves the value in the width it arrived in, because every
           use of it is a conversion that knows what to do with that width. A
           widening here would leave a value whose form no later instruction
           expects. */
        if (inst->op == IR_NEG) {
            emit_xmm_negate(encoder, width);
            return;
        }
        /* A floating value is true when it is not zero, so this asks whether it
           is zero. The comparison is against a zero in the second register
           rather than against the value itself, because a value always equals
           itself and the answer would then be the opposite of the one wanted.
           A value that is not a number compares unordered with everything and
           is true, so the equality has to exclude the unordered case, which the
           parity flag is what reports. */
        emit_xmm_convert_to_double(encoder, width);
        emit8(encoder, 0x66U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x57U); emit8(encoder, 0xc9U);  /* xorpd xmm1, xmm1 */
        emit8(encoder, 0x66U); emit8(encoder, 0x0fU);
        emit8(encoder, 0x2eU); emit8(encoder, 0xc1U);  /* ucomisd xmm0, xmm1 */
        emit8(encoder, 0x0fU); emit8(encoder, 0x94U);
        emit8(encoder, 0xc0U);                    /* sete al */
        emit8(encoder, 0x0fU); emit8(encoder, 0x9bU);
        emit8(encoder, 0xc1U);                    /* setnp cl */
        emit8(encoder, 0x20U); emit8(encoder, 0xc8U); /* and al, cl */
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U);
        emit8(encoder, 0xc0U);                    /* movzx eax, al */
        return;
    }
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
                       (((inst->flags) & IR_FLAG_SIGNED) != 0U));
    }
}

static void encode_conditional(IrEncoder *encoder, IrInst *inst)
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

/* Defined with the other wide helpers, below the point where the operand
   forms it needs have been written. */
static void encode_wide_cast(IrEncoder *encoder, IrInst *inst);

static void encode_cast(IrEncoder *encoder, IrInst *inst)
{
    size_t width = inst->width == 0U ? 8U : inst->width;
    bool to_wide = inst->type != NULL && inst->type->kind == TYPE_UINT128;
    bool from_wide = (inst->flags & IR_FLAG_WIDE_VALUE) != 0U;
    if (to_wide) {
        /* A conversion between two values of that type is a change of address,
           not of contents, so there is nothing to do. */
        if (from_wide) {
            encode_expr(encoder, inst->a);
            return;
        }
        encode_wide_cast(encoder, inst);
        return;
    }
    if (from_wide) {
        /* The value of a 128-bit expression is its address, and every use
           narrower than it wants the low half, which is the part a narrower
           integer can hold. */
        encode_expr(encoder, inst->a);
        emit_load_mem(encoder, 0U, 0U, 0, 8U, false);
        return;
    }
    if (is_float_type(inst->type)) {
        /* A conversion to the width the value already has changes nothing, and
           performing one anyway is not a no-op: widening then narrowing a
           single-precision value twice loses the low half of the double the
           first widening produced, because the second widening reads it back
           as if it were a single-precision value again. */
        if (inst->a != NULL && is_float_type(inst->a->type) &&
            type_size(inst->a->type) == type_size(inst->type)) {
            encode_expr(encoder, inst->a);
            return;
        }
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

static void encode_comma(IrEncoder *encoder, IrInst *inst)
{
    encode_expr(encoder, inst->a);
    encode_expr(encoder, inst->b);
}

/* A 128-bit integer operation.
 *
 * The value is sixteen bytes and no machine register is that wide, so both
 * operands and the result are addresses of storage. R10 and R11 hold the two
 * operand addresses; RAX and RDX hold the halves being combined; RCX carries
 * between them; and R9 holds the low half of a result that has to be written
 * before the high half is known.
 *
 * The machine's own multiply already leaves a full product in two registers,
 * so the wide product is the four products of the halves. Written out, with
 * A at R11 and B at R10, the high half is
 *
 *     A.hi*B.hi + floor(A.lo*B.lo / 2^64) + A.lo*B.hi + A.hi*B.lo
 *
 * which is the standard identity for a 128-bit product; the middle term is
 * the carry the machine produced. */
static void emit_alu_mem(IrEncoder *encoder, unsigned char opcode,
                         unsigned reg, unsigned base, int64_t displacement)
{
    emit8(encoder, 0x48U);
    emit8(encoder, opcode);
    emit_mem_reg(encoder, base, reg, displacement);
}

static void emit_shift_imm(IrEncoder *encoder, unsigned char digit,
                           unsigned reg, unsigned count)
{
    /* The opcode's reg field holds the operation, so the register being
       shifted is in the r/m field and needs the REX extension bit for the
       numbers above seven. */
    emit_rex(encoder, true, 0U, reg);
    emit8(encoder, 0xc1U);
    emit8(encoder, (unsigned char)(0xc0U | (digit << 3U) | (reg & 7U)));
    emit8(encoder, (unsigned char)count);
}

/* The two multiply forms the wide product needs: RAX times a memory operand
   with the full product in RDX:RAX, and one register times a memory operand
   keeping the low half. Both are the general register, wide form, so the
   opcode's reg field names the operation rather than a register. */
static void emit_mul_reg_mem(IrEncoder *encoder, unsigned base, int64_t displacement)
{
    emit_rex(encoder, true, 0U, base);
    emit8(encoder, 0xf7U);
    emit_mem_reg(encoder, base, 4U, displacement);
}

static void encode_wide_mul(IrEncoder *encoder, int64_t result)
{
    /* The low half is the product of the low halves. The high half is the
       high half of each of the four products of halves, of which the one
       whose low half was used above contributes its carry as well, so that
       carry is kept aside before the other three overwrite it.
       Written out, with A at R11 and B at R10, the high half is
           A.hi*B.hi + A.hi*B.lo + A.lo*B.hi + A.lo*B.lo
       taken eight bytes at a time from each, because each product is a full
       128-bit value and only its high half belongs here. */
    emit_load_mem(encoder, 0U, 11U, 0, 8U, false);
    emit_mul_reg_mem(encoder, 10U, 0);
    emit_mov_reg_reg(encoder, 8U, 0U);
    emit_mov_reg_reg(encoder, 1U, 2U);
    emit_load_mem(encoder, 0U, 11U, 8, 8U, false);
    emit_mul_reg_mem(encoder, 10U, 8);
    emit_mov_reg_reg(encoder, 9U, 2U);
    emit_load_mem(encoder, 0U, 11U, 8, 8U, false);
    emit_mul_reg_mem(encoder, 10U, 0);
    emit_alu(encoder, 0x01U, 9U, 2U);
    emit_load_mem(encoder, 0U, 11U, 0, 8U, false);
    emit_mul_reg_mem(encoder, 10U, 8);
    emit_alu(encoder, 0x01U, 9U, 2U);
    emit_alu(encoder, 0x01U, 9U, 1U);
    emit_store_partial(encoder, 8U, 5U, result, 8U);
    emit_store_partial(encoder, 9U, 5U, result + 8, 8U);
}

static void encode_wide(IrEncoder *encoder, IrInst *inst)
{
    int64_t result = inst->offset;
    encode_expr(encoder, inst->a);
    emit_push(encoder, 0U);
    ++encoder->stack_depth;
    encode_expr(encoder, inst->b);
    emit_pop(encoder, 11U);
    if (encoder->stack_depth != 0U) --encoder->stack_depth;
    emit_mov_reg_reg(encoder, 10U, 0U);
    if (inst->op == IR_WIDE_MUL) {
        encode_wide_mul(encoder, result);
        emit_lea_mem(encoder, 5U, 0U, result);
        return;
    }
    if (inst->op == IR_WIDE_SHL || inst->op == IR_WIDE_SHR) {
        /* Every path below leaves the low half in RAX and the high half in
           RCX, which is the order the result is written out in. */
        uint64_t count = inst->value.immediate;
        bool left = inst->op == IR_WIDE_SHL;
        if (count == 0U) {
            /* A shift of no bits is the value itself, so the halves are read
               rather than computed. */
            emit_load_mem(encoder, 0U, 11U, 0, 8U, false);
            emit_load_mem(encoder, 1U, 11U, 8, 8U, false);
        } else if (count >= 128U) {
            /* A shift of the whole width leaves nothing in either half. */
            emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
            emit8(encoder, 0x31U); emit8(encoder, 0xc9U);
        } else if (count >= 64U) {
            /* A shift of a whole eightbyte moves the value into the other
               half and leaves nothing behind in this one, so which half ends
               up empty depends on the direction. */
            emit_load_mem(encoder, left ? 1U : 0U, 11U, left ? 0 : 8, 8U, false);
            emit_shift_imm(encoder, left ? 4U : 5U, left ? 1U : 0U,
                           (unsigned)(count - 64U));
            if (left) {
                emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
            } else {
                emit8(encoder, 0x31U); emit8(encoder, 0xc9U);
            }
        } else if (left) {
            /* The new low half is the old one moved up; the bits it pushes
               past its end are the old high half moved up, plus what the old
               low half gave up at the top. */
            emit_load_mem(encoder, 9U, 11U, 0, 8U, false);
            emit_shift_imm(encoder, 4U, 9U, (unsigned)count);
            emit_load_mem(encoder, 0U, 11U, 8, 8U, false);
            emit_shift_imm(encoder, 4U, 0U, (unsigned)count);
            emit_load_mem(encoder, 1U, 11U, 0, 8U, false);
            emit_shift_imm(encoder, 5U, 1U, (unsigned)(64U - count));
            emit_alu(encoder, 0x01U, 0U, 1U);
            emit_mov_reg_reg(encoder, 1U, 0U);
            emit_mov_reg_reg(encoder, 0U, 9U);
        } else {
            /* The new high half is the old one moved down; the bits it gives
               up at the bottom are the old low half moved down. */
            emit_load_mem(encoder, 1U, 11U, 8, 8U, false);
            emit_shift_imm(encoder, 5U, 1U, (unsigned)count);
            emit_load_mem(encoder, 0U, 11U, 0, 8U, false);
            emit_shift_imm(encoder, 5U, 0U, (unsigned)count);
            emit_load_mem(encoder, 9U, 11U, 8, 8U, false);
            emit_shift_imm(encoder, 4U, 9U, (unsigned)(64U - count));
            emit_rex(encoder, true, 9U, 0U);
            emit8(encoder, 0x09U);
            emit_modrm_reg(encoder, 9U, 0U);
        }
        emit_store_partial(encoder, 0U, 5U, result, 8U);
        emit_store_partial(encoder, 1U, 5U, result + 8, 8U);
        emit_lea_mem(encoder, 5U, 0U, result);
        return;
    }
    unsigned char opcode = 0x31U;
    switch (inst->op) {
    case IR_WIDE_ADD: opcode = 0x03U; break;
    case IR_WIDE_SUB: opcode = 0x2bU; break;
    case IR_WIDE_AND: opcode = 0x23U; break;
    case IR_WIDE_OR: opcode = 0x0bU; break;
    case IR_WIDE_XOR: opcode = 0x33U; break;
    default: break;
    }
    for (unsigned half = 0U; half < 2U; ++half) {
        int64_t displacement = (int64_t)half * 8;
        emit_load_mem(encoder, 0U, 11U, displacement, 8U, false);
        emit_alu_mem(encoder, opcode, 0U, 10U, displacement);
        if (half == 0U) {
            emit_mov_reg_reg(encoder, 9U, 0U);
        } else if (inst->op == IR_WIDE_SUB) {
            /* A subtraction of the high half carries the borrow the low half
               produced, so it is a borrow-and-subtract here. */
            emit8(encoder, 0x48U); emit8(encoder, 0x19U);
            emit8(encoder, 0x5cU); emit8(encoder, 0x08U);
        }
    }
    emit_store_partial(encoder, 9U, 5U, result, 8U);
    emit_store_partial(encoder, 0U, 5U, result + 8, 8U);
    emit_lea_mem(encoder, 5U, 0U, result);
}

/* A conversion to or from the 128-bit type. Widening a narrower unsigned value
   fills the high half with zero; narrowing keeps the low half. A value of that
   type is an address, so a conversion that keeps the width passes the address
   on and a widening one leaves the address of the space it wrote. */
static void encode_wide_cast(IrEncoder *encoder, IrInst *inst)
{
    int64_t result = inst->offset;
    encode_expr(encoder, inst->a);
    emit_store_partial(encoder, 0U, 5U, result, 8U);
    emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    emit_store_partial(encoder, 0U, 5U, result + 8, 8U);
    emit_rex(encoder, true, 0U, 5U);
    emit8(encoder, 0x8dU);
    emit_mem_reg(encoder, 5U, 0U, result);
}

static void encode_expr(IrEncoder *encoder, IrInst *inst)
{
    if (inst == NULL || encoder->failed) return;
    switch (inst->op) {
    case IR_CONST: {
        unsigned width = inst->width == 0U ? 8U : (unsigned)inst->width;
        if (width < 8U && (((inst->flags) & IR_FLAG_SIGNED) != 0U)) {
            int64_t signed_value = (int64_t)inst->value.immediate;
            emit_mov_reg_imm(encoder, 0U, (uint64_t)signed_value, 8U);
        } else {
            emit_mov_reg_imm(encoder, 0U, inst->value.immediate, width);
        }
        break;
    }
    case IR_FLOAT_CONST: encode_float_constant(encoder, inst); break;
    case IR_LOAD: encode_load(encoder, inst); break;
    case IR_STORE: encode_store(encoder, inst); break;
    case IR_ADDR: encode_addr(encoder, inst); break;
    case IR_AGGREGATE: encode_expr(encoder, inst->a); break;
    case IR_WIDE_ADD: case IR_WIDE_SUB: case IR_WIDE_MUL: case IR_WIDE_AND:
    case IR_WIDE_OR: case IR_WIDE_XOR: case IR_WIDE_SHL: case IR_WIDE_SHR:
        encode_wide(encoder, inst);
        break;
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

static void encode_return(IrEncoder *encoder, IrInst *inst)
{
    if (inst->a != NULL) {
        encode_expr(encoder, inst->a);
        if (inst->type != NULL && argument_is_aggregate(inst->type)) {
            AggClass pieces[2];
            bool memory = false;
            size_t count = abi_aggregate_pieces(inst->type, pieces, &memory);
            if (count == 0U && !memory) {
                encoder_error(encoder, 4023U, &inst->location,
                              "return value cannot be passed under this ABI");
                return;
            }
            if (memory) {
                /* A result too large for the return registers is copied to the
                   space the caller named, and that address is the result. */
                const IrFunction *function = encoder->function;
                if (function == NULL || function->return_pointer_offset == 0) {
                    encoder_error(encoder, 4023U, &inst->location,
                                  "no return pointer for a memory result");
                    return;
                }
                emit_mov_reg_reg(encoder, 6U, 0U);
                emit_load_mem(encoder, 7U, 5U, function->return_pointer_offset,
                              8U, false);
                emit_mov_reg_reg(encoder, 11U, 7U);
                emit_object_copy(encoder, type_size(inst->type));
                emit_mov_reg_reg(encoder, 0U, 11U);
            } else {
                /* The result is an address; the eightbytes it covers go into
                   the return registers, in piece order. The address is copied
                   first so that loading the first piece does not lose the
                   rest. */
                emit_mov_reg_reg(encoder, 11U, 0U);
                static const unsigned char return_general[2] = {0U, 2U};
                size_t remaining = type_size(inst->type);
                for (size_t piece = 0U; piece < count; ++piece) {
                    size_t width = remaining >= 8U ? 8U : remaining;
                    if (pieces[piece] == AGG_SSE) {
                        emit_load_piece_vector(encoder, (unsigned)piece, 11U,
                                               (int64_t)piece * 8, width);
                    } else {
                        emit_load_partial(encoder, return_general[piece], 11U,
                                          (int64_t)piece * 8, width);
                    }
                    remaining -= width;
                }
            }
        } else if (inst->type != NULL && !type_is_void(inst->type)) {
            emit_normalize(encoder, type_size(inst->type), type_is_signed(inst->type));
        }
    }
    emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit_modrm_reg(encoder, 5U, 4U);
    emit_pop(encoder, 5U);
    emit8(encoder, 0xc3U);
}

static void encode_statement(IrEncoder *encoder, IrInst *inst)
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

/* Move the pieces of an aggregate parameter from the registers it arrived in
   into the frame, and leave a parameter that arrived on the stack alone,
   because a callee may read and write its own parameters in place. */
static void emit_parameter_pieces(IrEncoder *encoder, const ArgSlot *slot,
                                  const Type *type, int64_t offset)
{
    size_t remaining = type_size(type);
    for (unsigned piece = 0U; piece < slot->pieces; ++piece) {
        size_t width = remaining >= 8U ? 8U : remaining;
        if (slot->classes[piece] == AGG_SSE) {
            emit_store_piece_vector(encoder, slot->registers[piece], 5U,
                                    offset + (int64_t)piece * 8, width);
        } else {
            emit_store_partial(encoder, argument_registers[slot->registers[piece]],
                               5U, offset + (int64_t)piece * 8, width);
        }
        remaining -= width;
    }
}

static void emit_prologue(IrEncoder *encoder, const IrFunction *function)
{
    emit_push(encoder, 5U);
    emit_mov_reg_reg(encoder, 5U, 4U);
    if (function->frame_size != 0U) {
        emit8(encoder, 0x48U); emit8(encoder, 0x81U); emit8(encoder, 0xecU);
        emit32(encoder, (uint32_t)function->frame_size);
    }
    /* A result too large for the return registers is written through a pointer
       the caller passes in the first general register, which the frame keeps
       for the return to use. */
    if (function->return_pointer_offset != 0) {
        emit_store_mem(encoder, argument_registers[0], 5U,
                       function->return_pointer_offset, 8U);
    }
    size_t count = 0U;
    for (Symbol *parameter = function->symbol->type->parameters; parameter != NULL;
         parameter = parameter->next) {
        ++count;
    }
    if (count == 0U) {
        return;
    }
    Type **types = cc64_xcalloc(count, sizeof(*types));
    ArgSlot *slots = cc64_xcalloc(count, sizeof(*slots));
    if (types == NULL || slots == NULL) {
        free(types);
        free(slots);
        encoder_error(encoder, 4020U, NULL, "cannot record parameter placement");
        return;
    }
    size_t index = 0U;
    for (Symbol *parameter = function->symbol->type->parameters; parameter != NULL;
         parameter = parameter->next, ++index) {
        types[index] = parameter->type;
    }
    AggClass return_pieces[2];
    bool returns_memory = false;
    if (function->symbol->type->return_type != NULL &&
        argument_is_aggregate(function->symbol->type->return_type)) {
        (void)abi_aggregate_pieces(function->symbol->type->return_type,
                                   return_pieces, &returns_memory);
    }
    (void)abi_assign_arguments(types, index, returns_memory, slots);
    index = 0U;
    for (Symbol *parameter = function->symbol->type->parameters; parameter != NULL;
         parameter = parameter->next, ++index) {
        const ArgSlot *slot = &slots[index];
        if (!slot->ok || !slot->in_register) {
            continue;
        }
        if (argument_is_aggregate(parameter->type)) {
            emit_parameter_pieces(encoder, slot, parameter->type,
                                  (int64_t)parameter->offset);
        } else if (slot->classes[0] == AGG_SSE) {
            emit_store_piece_vector(encoder, slot->registers[0], 5U,
                                    (int64_t)parameter->offset,
                                    type_size(parameter->type));
        } else {
            /* The slot is as wide as the parameter and no wider, so the move
               is too: a wider one would write past the slot and over the
               saved frame pointer. */
            emit_store_partial(encoder, argument_registers[slot->registers[0]],
                               5U, (int64_t)parameter->offset,
                               type_size(parameter->type));
        }
    }
    free(types);
    free(slots);
    /* A variadic function also spills the whole integer argument register set
       so that va_start can walk the unnamed arguments. Floating arguments are
       not part of the version 1 variadic contract. */
    if (function->symbol->type->variadic && function->va_area_offset != 0) {
        unsigned named = 0U;
        for (Symbol *parameter = function->symbol->type->parameters;
             parameter != NULL; parameter = parameter->next) {
            if (!is_float_type(parameter->type) && named < 6U) ++named;
        }
        for (unsigned i = 0U; i < 6U; ++i) {
            emit_store_mem(encoder, argument_registers[i], 5U,
                           function->va_area_offset + (int64_t)i * 8, 8U);
        }
        /* Unnamed arguments past the sixth did not arrive in registers. The
           walk in va_arg is a flat eight-byte step from the save area, so the
           incoming stack area is copied into the slots that follow it and the
           walk carries on across the boundary unchanged. Slots the caller did
           not fill are copied but never read, because a caller only ever reads
           the arguments it passed. The copy is unrolled: a loop would need
           labels, and the encoder's own label ids share a space with the
           per-function ids the lowering hands out. */
        int64_t source = 16 + (int64_t)(named > 6U ? named - 6U : 0U) * 8;
        for (unsigned slot = 0U; slot < CC64_VARIADIC_SLOTS; ++slot) {
            int64_t from = source + (int64_t)slot * 8;
            int64_t to = function->va_area_offset + 48 + (int64_t)slot * 8;
            emit8(encoder, 0x48U); emit8(encoder, 0x8bU); emit8(encoder, 0x85U);
            emit32(encoder, (uint32_t)from);
            emit8(encoder, 0x48U); emit8(encoder, 0x89U); emit8(encoder, 0x85U);
            emit32(encoder, (uint32_t)to);
        }
    }
}

/* A defined object needs a section, a size, and, for an object without an
   initializer, its place in the zero-filled section. Its record may already
   exist because a function that was encoded earlier referred to it, so the
   record is filled in rather than appended. */
static bool prepare_global_symbol(IrEncoder *encoder, Symbol *symbol)
{
    if (symbol == NULL || symbol->name == NULL) return true;
    if (symbol->storage == STORAGE_EXTERN && symbol->initializer == NULL) {
        return object_symbol_index(encoder, symbol) != UINT32_MAX;
    }
    size_t index = object_symbol_index(encoder, symbol);
    if (index == UINT32_MAX) return false;
    bool initialized = symbol->initializer != NULL;
    ObjectSymbol *entry = &encoder->builder->symbols[index];
    entry->section_index = initialized ? 1U : 2U;
    entry->kind = 1U;
    entry->binding = (unsigned char)(symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U);
    entry->size = type_size(symbol->type);
    entry->defined = true;
    if (!initialized) {
        size_t alignment = type_alignment(symbol->type);
        size_t mask = alignment == 0U ? 0U : alignment - 1U;
        size_t offset = (encoder->bss->size + mask) & ~mask;
        entry->value = offset;
        encoder->bss->size = offset + entry->size;
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

static bool write_constant_initializer(IrEncoder *encoder, AstNode *value,
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

static bool append_global_data(IrEncoder *encoder, const IrProgram *program)
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
    RUNTIME_DELETE,
    RUNTIME_EXIT,
    RUNTIME_START,
    RUNTIME_CONSOLE_READY,
    RUNTIME_CONSOLE_LINE,
    RUNTIME_TIME_FIELDS,
    RUNTIME_DATE_FIELDS
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
    else if (strcmp(name, "cc64_delete") == 0) *function = RUNTIME_DELETE;
    else if (strcmp(name, "cc64_exit") == 0) *function = RUNTIME_EXIT;
    else if (strcmp(name, "cc64_start") == 0) *function = RUNTIME_START;
    else if (strcmp(name, "cc64_console_ready") == 0) *function = RUNTIME_CONSOLE_READY;
    else if (strcmp(name, "cc64_console_line") == 0) *function = RUNTIME_CONSOLE_LINE;
    else if (strcmp(name, "cc64_time_fields") == 0) *function = RUNTIME_TIME_FIELDS;
    else if (strcmp(name, "cc64_date_fields") == 0) *function = RUNTIME_DATE_FIELDS;
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
#define CC64_VARIADIC_COPY 14U
#define CC64_VARIADIC_COPIED 15U
#define CC64_START_ARGUMENTS 32
#define CC64_START_TAIL 144
/* The invocation name is copied to the frame immediately below the tail copy
   and separated from it by one space, so the tokenizer that splits the tail
   also yields the name as the first argument. The control block stores the name
   as eight stem bytes followed by three extension bytes, both space padded, so
   the stem is copied alone: the padding then separates it from the extension
   and from the tail, and the tokenizer trims the padding for free. */
#define CC64_START_NAME 9
#define CC64_START_NAME_AT (-0xa0 - CC64_START_NAME)
/* Frame layout below the frame pointer: the command tail copy occupies the
   upper part and the argument vector the lower part, so neither can overwrite
   the other or the saved frame pointer. The vector holds one more slot than the
   argument limit, because the terminator is written past the last entry, and it
   is placed far enough below the name that a full vector still ends below it. */
#define CC64_START_TAIL_AT (-0xa0)
#define CC64_START_VECTOR_AT (-0x200)
#define CC64_START_FRAME 0x210
#define CC64_START_SKIP_LENGTH 1U
#define CC64_START_SKIP_SPACE 2U
#define CC64_START_RECORD 3U
#define CC64_START_SCAN 4U
#define CC64_START_SPLIT 5U
#define CC64_START_DONE 6U
#define CC64_START_COPY 7U
#define CC64_START_COPIED 8U
#define CC64_START_NAME_COPY 9U
#define CC64_START_NAME_COPIED 10U
#define CC64_START_NO_NAME 11U
#define CC64_START_CALL 12U
#define CC64_START_SKIP_DELIMITER 13U

static void emit_startup_body(IrEncoder *encoder, Symbol *main_symbol)
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
    /* The tail copy is an explicit byte loop. A repeated-string move depends
       on the direction flag reaching this code clear, and this routine runs
       before any library code has had a chance to establish that; the loop
       removes the dependency entirely. */
    define_label(encoder, CC64_START_COPY);
    emit8(encoder, 0x48U); emit8(encoder, 0x85U); emit8(encoder, 0xc9U);
    emit_conditional_jump(encoder, 4U, CC64_START_COPIED);
    emit8(encoder, 0x8aU); emit8(encoder, 0x07U);       /* mov al, [rdi] */
    emit8(encoder, 0x88U); emit8(encoder, 0x06U);       /* mov [rsi], al */
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc7U);
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc6U);
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc9U);
    emit_jump(encoder, CC64_START_COPY);
    define_label(encoder, CC64_START_COPIED);
    emit8(encoder, 0xc6U); emit8(encoder, 0x04U);
    emit8(encoder, 0x06U); emit8(encoder, 0x00U);       /* byte [rsi+rax] = 0 */
    /* Copy the invocation name from the first file control block in the
       process prefix and terminate it directly below the tail copy. */
    emit_lea_mem(encoder, 5U, 6U, CC64_START_NAME_AT);   /* lea rsi, name */
    emit_lea_mem(encoder, 10U, 7U, 0x61U);              /* lea rdi, name */
    emit_mov_reg_imm(encoder, 1U, CC64_START_NAME - 1U, 8U);
    define_label(encoder, CC64_START_NAME_COPY);
    emit8(encoder, 0x48U); emit8(encoder, 0x85U); emit8(encoder, 0xc9U);
    emit_conditional_jump(encoder, 4U, CC64_START_NAME_COPIED);
    emit8(encoder, 0x8aU); emit8(encoder, 0x07U);       /* mov al, [rdi] */
    emit8(encoder, 0x88U); emit8(encoder, 0x06U);       /* mov [rsi], al */
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc7U);
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc6U);
    emit8(encoder, 0x48U); emit8(encoder, 0xffU); emit8(encoder, 0xc9U);
    emit_jump(encoder, CC64_START_NAME_COPY);
    define_label(encoder, CC64_START_NAME_COPIED);
    /* A separator follows the name, because the tail copy starts immediately
       after it and the two would otherwise run together. The pass below then
       tokenizes the name and the arguments as one string. */
    emit8(encoder, 0xc6U); emit8(encoder, 0x06U);
    emit8(encoder, 0x20U);                              /* byte [rsi] = ' ' */
    /* An empty name means the caller supplied no invocation name. The vector
       is then passed empty rather than shifted, so a program that expects a
       name observes the missing argument instead of a wrong one. */
    emit8(encoder, 0x40U); emit8(encoder, 0x0fU); emit8(encoder, 0xb6U);
    emit8(encoder, 0x85U);
    emit32(encoder, (uint32_t)(int32_t)CC64_START_NAME_AT);
    emit8(encoder, 0x84U); emit8(encoder, 0xc0U);       /* test al, al */
    emit_conditional_jump(encoder, 4U, CC64_START_NO_NAME);
    emit_lea_mem(encoder, 5U, 2U, CC64_START_VECTOR_AT);  /* lea rdx */
    emit8(encoder, 0x45U); emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    emit_lea_mem(encoder, 5U, 11U, CC64_START_NAME_AT);   /* lea r11, name */
    /* Split the copy in place. The terminating NUL ends the scan, so no
       length is tracked and no read can pass the end of the buffer. */
    define_label(encoder, CC64_START_SKIP_SPACE);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x00U);                              /* cmp byte [r11], 0 */
    emit_conditional_jump(encoder, 4U, CC64_START_DONE);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x20U);                              /* cmp byte [r11], ' ' */
    emit_conditional_jump(encoder, 4U, CC64_START_SKIP_DELIMITER);
    emit8(encoder, 0x41U); emit8(encoder, 0x80U); emit8(encoder, 0x3bU);
    emit8(encoder, 0x09U);                              /* cmp byte [r11], tab */
    emit_conditional_jump(encoder, 5U, CC64_START_RECORD);
    define_label(encoder, CC64_START_SKIP_DELIMITER);
    emit8(encoder, 0x49U); emit8(encoder, 0xffU); emit8(encoder, 0xc3U);
    emit_jump(encoder, CC64_START_SKIP_SPACE);
    define_label(encoder, CC64_START_RECORD);
    emit8(encoder, 0x4eU); emit8(encoder, 0x89U); emit8(encoder, 0x1cU);
    emit8(encoder, 0xc2U);                             /* mov [rdx+r8*8], r11 */
    /* The count is bounded at the documented argument limit, so the vector can
       never grow past its slots: the terminator is written one past the last
       entry, and the loop stops before another entry is stored. */
    emit8(encoder, 0x41U); emit8(encoder, 0x83U);
    emit8(encoder, 0xf8U);
    emit8(encoder, (unsigned char)(CC64_START_ARGUMENTS - 1U));
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
    emit_jump(encoder, CC64_START_CALL);
    define_label(encoder, CC64_START_NO_NAME);
    emit8(encoder, 0x31U); emit8(encoder, 0xffU);        /* xor edi, edi */
    emit8(encoder, 0x45U); emit8(encoder, 0x31U); emit8(encoder, 0xc0U);
    define_label(encoder, CC64_START_CALL);
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

/* A target service reports failure by setting CF and leaving a small positive
   code in RAX. Left alone, that code is indistinguishable from a valid result:
   error 6 from a file read is a plausible handle, error 25 from a seek is a
   plausible offset, and the max-free count from a failed allocation is a
   plausible address. Every service thunk therefore ends with one fixed rule,
   which is what the runtime library is written against: the service value on
   success, -1 on failure. CF survives the interrupt return in the trap frame's
   RFLAGS slot, so a forward conditional jump over the assignment is enough. */
static void emit_service_int(IrEncoder *encoder)
{
    emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
    emit8(encoder, 0x73U); emit8(encoder, 0x07U);   /* jnc past the assignment */
    emit8(encoder, 0x48U); emit8(encoder, 0xc7U); emit8(encoder, 0xc0U);
    emit32(encoder, 0xffffffffU);                  /* mov rax, -1 */
}

static void emit_runtime_body(IrEncoder *encoder, RuntimeFunction function)
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
        emit_service_int(encoder);
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
        emit_service_int(encoder);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_FREE:
        emit_push(encoder, 3U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x4900U, 4U);
        emit_service_int(encoder);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_OPEN:
        emit_push(encoder, 3U);
        emit8(encoder, 0x31U); emit8(encoder, 0xd2U);
        emit_mov_reg_reg(encoder, 2U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x3d00U, 4U);
        emit_service_int(encoder);
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
        emit_service_int(encoder);
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
        emit_service_int(encoder);
        break;
    case RUNTIME_CLOSE:
        emit_push(encoder, 3U);
        emit_mov_reg_reg(encoder, 3U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x3e00U, 4U);
        emit_service_int(encoder);
        emit_pop(encoder, 3U);
        break;
    case RUNTIME_DELETE:
        /* cc64_delete(const void *fcb): RDX=file control block, AH=13h. The
           target marks the directory entry and frees the cluster chain, so the
           file's space is returned to the volume. */
        emit_mov_reg_reg(encoder, 2U, 7U);
        emit_mov_reg_imm(encoder, 0U, 0x1300U, 4U);
        emit_service_int(encoder);
        break;
    case RUNTIME_EXIT:
        emit_mov_reg32_reg(encoder, 0U, 7U);
        emit8(encoder, 0xb4U); emit8(encoder, 0x4cU);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        break;
    case RUNTIME_CONSOLE_READY:
        /* cc64_console_ready(void): AH=0Bh, AL=FF when a character is waiting.
           The service reports availability in the low byte and leaves the
           carry clear either way, so the byte is normalized to one and zero
           here rather than being taken for a count. */
        emit_mov_reg_imm(encoder, 0U, 0x0b00U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        emit8(encoder, 0x85U); emit8(encoder, 0xc0U);
        emit8(encoder, 0x0fU); emit8(encoder, 0x95U); emit8(encoder, 0xc0U);
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U); emit8(encoder, 0xc0U);
        break;
    case RUNTIME_CONSOLE_LINE:
        /* cc64_console_line(void): AH=03h reads the console's own line, and is
           the service the target's shell uses to get a character from it. The
           carry says whether one arrived, so the branch below turns the pair
           into a value: the character when the carry is clear, and -1 when it
           is set, which is a character no encoding produces. The read cannot be
           blocking, so a caller that asks with nothing waiting is told so
           rather than left waiting. */
        emit_mov_reg_imm(encoder, 0U, 0x0300U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit8(encoder, 0x72U); emit8(encoder, 0x04U);   /* jb .line_empty */
        emit8(encoder, 0x0fU); emit8(encoder, 0xb6U);
        emit8(encoder, 0xc0U);                          /* movzx eax, al */
        emit8(encoder, 0xc3U);                          /* ret */
        emit8(encoder, 0xb8U); emit8(encoder, 0xffU);   /* .line_empty: */
        emit8(encoder, 0xffU); emit8(encoder, 0xffU);
        emit8(encoder, 0xffU);                          /* mov eax, -1 */
        break;
    case RUNTIME_TIME_FIELDS:
        /* cc64_time_fields(void): AH=2Ch leaves RCX=(hour<<8)|minute and
           RDX=second, and the value returned is the three fields at byte 2,
           byte 1 and byte 0. RCX already holds hour and minute in the order
           wanted, so it is lifted one byte to make room for the seconds, not
           shifted down and rejoined. Each half is taken 32 bits wide because
           the service leaves the rest of the register to the caller, so the
           bits above the fields are not the clock's. */
        emit_mov_reg_imm(encoder, 0U, 0x2c00U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit8(encoder, 0x31U); emit8(encoder, 0xc0U);   /* xor eax, eax */
        emit8(encoder, 0x89U); emit8(encoder, 0xc8U);   /* mov eax, ecx */
        emit8(encoder, 0x48U); emit8(encoder, 0xc1U);
        emit8(encoder, 0xe0U); emit8(encoder, 0x08U); /* shl rax, SHIFT */
        emit8(encoder, 0x8bU); emit8(encoder, 0xfaU);   /* mov edi, edx */
        emit8(encoder, 0xc1U); emit8(encoder, 0xefU);
        emit8(encoder, 0x10U);                          /* shr edi, 16 */
        emit8(encoder, 0x48U); emit8(encoder, 0x09U);
        emit8(encoder, 0xf8U);                          /* or rax, rdi */
        break;
    case RUNTIME_DATE_FIELDS:
        /* cc64_date_fields(void): AH=2Ah leaves RCX=year and
           RDX=(month<<8)|day, and the value returned is the three fields at
           byte 2, byte 1 and byte 0. RDX already holds month and day in the
           order wanted, so the year is lifted two bytes above it rather than
           the halves being shifted together. Each half is taken 32 bits wide
           because the service leaves the rest of the register to the caller,
           so the bits above the fields are not the clock's. */
        emit_mov_reg_imm(encoder, 0U, 0x2a00U, 4U);
        emit8(encoder, 0xcdU); emit8(encoder, 0x21U);
        emit8(encoder, 0x31U); emit8(encoder, 0xc0U);   /* xor eax, eax */
        emit8(encoder, 0x89U); emit8(encoder, 0xc8U);   /* mov eax, ecx */
        emit8(encoder, 0x48U); emit8(encoder, 0xc1U);
        emit8(encoder, 0xe0U); emit8(encoder, 0x10U); /* shl rax, SHIFT */
        emit8(encoder, 0x8bU); emit8(encoder, 0xfaU);   /* mov edi, edx */
        emit8(encoder, 0xc1U); emit8(encoder, 0xefU);
        emit8(encoder, 0x10U);                          /* shr edi, 16 */
        emit8(encoder, 0x48U); emit8(encoder, 0x09U);
        emit8(encoder, 0xf8U);                          /* or rax, rdi */
        break;
    case RUNTIME_START:
        return;
    }
    emit8(encoder, 0xc3U);
}

static bool append_runtime_functions(IrEncoder *encoder)
{
    static const char *const names[] = {
        "cc64_putc", "cc64_write", "cc64_read", "cc64_alloc",
        "cc64_free", "cc64_open", "cc64_create", "cc64_lseek",
        "cc64_close", "cc64_delete", "cc64_exit", "cc64_start",
        "cc64_console_ready", "cc64_console_line",
        "cc64_time_fields", "cc64_date_fields"
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
        IrEncoder runtime = {0};
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

static bool encode_function(IrEncoder *encoder, const IrFunction *function)
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

IrEncoder *ir_encoder_create(Arena *arena, ObjectBuilder *builder,
                             DiagnosticSink *diagnostics)
{
    IrEncoder *encoder = cc64_xmalloc(sizeof(*encoder));
    if (encoder == NULL) return NULL;
    memset(encoder, 0, sizeof(*encoder));
    encoder->arena = arena;
    encoder->diagnostics = diagnostics;
    encoder->builder = builder;
    encoder->text = object_add_section(builder, ".text", 0U, 16U);
    encoder->data = object_add_section(builder, ".data", 2U, 8U);
    encoder->bss = object_add_section(builder, ".bss", 3U, 8U);
    if (encoder->text == NULL || encoder->data == NULL || encoder->bss == NULL) {
        free(encoder);
        return NULL;
    }
    /* Adding a section can move the table, so the section records are read back
       from the builder rather than kept from the calls above. No section is
       added after this point, so the three stay put. */
    encoder->text = &builder->sections[0];
    encoder->data = &builder->sections[1];
    encoder->bss = &builder->sections[2];
    return encoder;
}

bool ir_encoder_add_function(IrEncoder *encoder, const IrFunction *function)
{
    if (function == NULL || function->symbol == NULL ||
        function->symbol->name == NULL) return false;
    Symbol *symbol = function->symbol;
    if (strcmp(symbol->name, "main") == 0) encoder->main_symbol = symbol;
    /* A function that an earlier function already referred to has a record
       without a section; this is where that record becomes a definition. */
    if (object_symbol_index(encoder, symbol) == UINT32_MAX) return false;
    for (size_t i = 0U; i < encoder->builder->symbol_count; ++i) {
        if (encoder->builder->symbols[i].source_symbol == symbol) {
            encoder->builder->symbols[i].section_index = 0U;
            encoder->builder->symbols[i].kind = 2U;
            encoder->builder->symbols[i].binding =
                (unsigned char)(symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U);
            break;
        }
    }
    return encode_function(encoder, function);
}

bool ir_encoder_finish(IrEncoder *encoder, const IrProgram *program)
{
    for (size_t i = 0U; i < program->global_count; ++i) {
        if (!prepare_global_symbol(encoder, program->global_symbols[i])) return false;
    }
    if (!append_global_data(encoder, program)) return false;
    if (!append_runtime_functions(encoder)) return false;
    return !encoder->failed;
}

void ir_encoder_destroy(IrEncoder *encoder)
{
    if (encoder == NULL) return;
    free(encoder->labels);
    free(encoder->fixups);
    free(encoder->code);
    free(encoder);
}
