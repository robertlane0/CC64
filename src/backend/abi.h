#ifndef CC64_ABI_H
#define CC64_ABI_H

#include "semantic/semantic.h"

/* How one eightbyte of an aggregate is carried.
 *
 * A struct or union passed or returned by value is split into eightbyte
 * pieces, and each piece is named by what it holds. A piece holding any
 * integer, pointer, or bit-field travels in a general register; a piece
 * holding only floating values travels in a vector register. When both kinds
 * share a piece the general register wins, because a piece the vector
 * register cannot express must go somewhere and the general register always
 * can.
 */
typedef enum AggClass {
    AGG_INTEGER,
    AGG_SSE
} AggClass;

/* Classify a struct or union.
 *
 * `pieces` receives one class per eightbyte and must have room for two. The
 * return value is the number of eightbytes the object occupies, or zero when
 * the type is not an aggregate this ABI can describe at all. `memory` is set
 * when the object is a complete aggregate too large or too awkward for the
 * register path; such an object is passed in the argument area, and returned
 * through a pointer the caller supplies.
 *
 * Two shapes go to memory. An object of more than two eightbytes has more
 * bytes than the four registers this convention has for a value. A field that
 * does not both start and end inside one eightbyte cannot be delivered by a
 * register at all, so the object as a whole goes to memory rather than being
 * split at a field the split would cut in half.
 */
size_t abi_aggregate_pieces(const Type *type, AggClass *pieces, bool *memory);

/* Where one argument of a call is carried.
 *
 * An argument is either in registers, in the outgoing argument area of the
 * call, or in memory in the sense above. A register argument names one
 * register per eightbyte it occupies; an argument in the outgoing area names
 * the byte offset it starts at, measured from the return address, so a callee
 * can find it at that offset from its own frame pointer once the return
 * address and the saved frame pointer are in place.
 *
 * `pieces` is zero for a scalar and one or two for an aggregate carried in
 * registers. `ok` is false when the type cannot be passed under this ABI at
 * all, in which case the caller diagnoses it rather than passing something
 * else.
 */
typedef struct ArgSlot {
    bool ok;
    bool in_register;
    bool memory;                /* the whole object goes to memory */
    unsigned pieces;
    AggClass classes[2];
    unsigned registers[2];      /* register number within its class */
    size_t stack_offset;        /* bytes past the return address */
    size_t size;                /* bytes the argument occupies */
} ArgSlot;

/* Assign every argument in a list, in order, and return the total size of the
   outgoing argument area in bytes. `types` holds one type per argument and
   `slots` receives one filled slot per argument.
 *
 * `returns_memory` says the call's result is a memory-class aggregate, which
 * the caller passes a pointer to in the first general register; the named
 * arguments then begin at the second. */
size_t abi_assign_arguments(Type *const *types, size_t count, bool returns_memory,
                            ArgSlot *slots);

#endif
