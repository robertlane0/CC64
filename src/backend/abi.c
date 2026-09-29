#include "backend/abi.h"

#include <string.h>

/* An aggregate is passed as a sequence of eightbytes, and what decides where
   each one goes is what the bytes it covers hold. A struct is a list of
   members at known offsets, so walking the members and noting which eightbyte
   each scalar lands in describes the whole object; a union's members share one
   offset, and taking the widest of them covers every byte.
 *
 * The rules are derived here from the layout the compiler already computes,
   rather than from a description of any other convention: a field of eight
   bytes or less that starts on an eightbyte boundary occupies exactly that
   eightbyte, and a field that would cross a boundary, or an object too large
   for two eightbytes, is carried in memory instead. */

static bool is_vector_type(const Type *type)
{
    return type != NULL && (type->kind == TYPE_FLOAT || type->kind == TYPE_DOUBLE);
}

/* Record the class of one scalar at `offset`. A piece that has not been named
   yet takes the class of the first field that reaches it, and a later field
   promotes it to the general register, which can carry anything a vector
   register can and more. `seen` is what tells "nothing has reached this piece"
   apart from "a general field is here", which are otherwise the same value. */
static void mark_piece(const Type *type, size_t offset, AggClass *pieces,
                       bool *seen)
{
    size_t index = offset / 8U;
    if (index >= 2U) {
        return;
    }
    AggClass wanted = is_vector_type(type) ? AGG_SSE : AGG_INTEGER;
    if (wanted == AGG_INTEGER || !seen[index]) {
        pieces[index] = wanted;
        seen[index] = true;
    }
}

static bool walk_members(const Type *type, size_t base, AggClass *pieces,
                         bool *seen)
{
    for (const Member *member = type->members; member != NULL;
         member = member->next) {
        const Type *field = member->type;
        size_t offset = base + member->offset;
        if (field == NULL) {
            return false;
        }
        if (field->kind == TYPE_STRUCT || field->kind == TYPE_UNION) {
            if (!walk_members(field, offset, pieces, seen)) {
                return false;
            }
            continue;
        }
        if (field->kind == TYPE_ARRAY) {
            /* A nested array is a run of elements; every element that is not
               itself an aggregate contributes one whole scalar per stride. */
            size_t element = type_size(field->base);
            if (element == 0U) {
                continue;
            }
            for (size_t index = 0U; index < field->array_count; ++index) {
                if (!walk_members(field->base, offset + index * element, pieces,
                                  seen)) {
                    return false;
                }
            }
            continue;
        }
        size_t size = type_size(field);
        if (size == 0U) {
            continue;
        }
        /* A field that does not lie inside one eightbyte cannot be read from
           a register, so the object is carried in memory instead. */
        if ((offset % 8U) + size > 8U) {
            return false;
        }
        mark_piece(field, offset, pieces, seen);
    }
    return true;
}

size_t abi_aggregate_pieces(const Type *type, AggClass *pieces, bool *memory)
{
    if (type == NULL || pieces == NULL || memory == NULL) {
        return 0U;
    }
    *memory = false;
    if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
        return 0U;
    }
    if (type->incomplete) {
        return 0U;
    }
    size_t size = type_size(type);
    if (size == 0U) {
        return 0U;
    }
    if (size > 16U) {
        *memory = true;
        return 0U;
    }
    bool seen[2] = {false, false};
    if (!walk_members(type, 0U, pieces, seen)) {
        *memory = true;
        return 0U;
    }
    /* Bytes the members do not describe are padding. They travel in a general
       register, which is the class that can carry an arbitrary byte pattern,
       so a padding-only piece is a general one. */
    for (size_t piece = 0U; piece < (size + 7U) / 8U; ++piece) {
        if (!seen[piece]) pieces[piece] = AGG_INTEGER;
    }
    return (size + 7U) / 8U;
}

static size_t round_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1U;
    if (value > SIZE_MAX - mask) {
        return SIZE_MAX;
    }
    return (value + mask) & ~mask;
}

/* An argument is placed in a register only when every eightbyte it needs has a
   register left. A register that is available for one eightbyte but not for
   the next cannot be used, because the two halves of an aggregate are not
   separable: a callee that reads one eightbyte and finds the other in the
   outgoing area would be reading a different value than the caller wrote. */
static bool take_registers(const AggClass *classes, unsigned pieces,
                           unsigned *general, unsigned *vector,
                           unsigned *registers)
{
    unsigned need_general = 0U;
    unsigned need_vector = 0U;
    for (unsigned i = 0U; i < pieces; ++i) {
        if (classes[i] == AGG_SSE) ++need_vector;
        else ++need_general;
    }
    if (*general + need_general > 6U || *vector + need_vector > 8U) {
        return false;
    }
    for (unsigned i = 0U; i < pieces; ++i) {
        if (classes[i] == AGG_SSE) registers[i] = (*vector)++;
        else registers[i] = (*general)++;
    }
    return true;
}

size_t abi_assign_arguments(Type *const *types, size_t count, bool returns_memory,
                            ArgSlot *slots)
{
    /* A memory-class result takes the first general register for the pointer
       the caller supplies, so the named arguments start after it. */
    unsigned general = returns_memory ? 1U : 0U;
    unsigned vector = 0U;
    size_t stack = 0U;
    for (size_t i = 0U; i < count; ++i) {
        ArgSlot *slot = &slots[i];
        memset(slot, 0, sizeof(*slot));
        slot->ok = true;
        const Type *type = types[i];
        if (type == NULL) {
            slot->ok = false;
            continue;
        }
        if (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
            unsigned pieces = (unsigned)abi_aggregate_pieces(type, slot->classes,
                                                            &slot->memory);
            if (pieces == 0U && !slot->memory) {
                slot->ok = false;
                continue;
            }
            slot->size = round_up(type_size(type), 8U);
            if (slot->size == SIZE_MAX) {
                slot->ok = false;
                return 0U;
            }
            if (pieces != 0U) {
                slot->pieces = pieces;
                if (take_registers(slot->classes, pieces, &general, &vector,
                                   slot->registers)) {
                    slot->in_register = true;
                    continue;
                }
            }
            stack = round_up(stack, 8U);
            slot->stack_offset = stack;
            stack += slot->size;
            continue;
        }
        slot->size = 8U;
        if (type->kind == TYPE_FLOAT || type->kind == TYPE_DOUBLE) {
            if (vector < 8U) {
                slot->in_register = true;
                slot->classes[0] = AGG_SSE;
                slot->pieces = 1U;
                slot->registers[0] = vector++;
                continue;
            }
        } else if (general < 6U) {
            slot->in_register = true;
            slot->classes[0] = AGG_INTEGER;
            slot->pieces = 1U;
            slot->registers[0] = general++;
            continue;
        }
        stack = round_up(stack, 8U);
        slot->stack_offset = stack;
        stack += 8U;
    }
    return round_up(stack, 8U);
}
