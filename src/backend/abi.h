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
 * can. AGG_MEMORY names the case this ABI revision does not define: an
 * aggregate that needs more room than two registers can be given.
 */
typedef enum AggClass {
    AGG_INTEGER,
    AGG_SSE
} AggClass;

/* The number of eightbytes an aggregate occupies under this ABI, or zero when
   the type is not an aggregate. `pieces` receives one class per eightbyte and
   must have room for two.
 *
 * A type larger than two eightbytes, one whose fields do not land inside them
 * cleanly, or one that is not yet complete is reported as zero. The caller
 * treats that as "this revision cannot pass it" and diagnoses it, rather than
 * passing a partial value.
 */
size_t abi_aggregate_pieces(const Type *type, AggClass *pieces);

/* Where one argument of a call is carried.
 *
 * An argument is either in registers or in the outgoing argument area of the
 * call. A register argument names one register per eightbyte it occupies; a
 * stack argument names the byte offset it starts at, measured from the return
 * address, so a callee can find it at that offset from its own frame pointer
 * once the return address and the saved frame pointer are in place.
 *
 * `pieces` is zero for a scalar and one or two for an aggregate. `ok` is false
 * when the type cannot be passed under this ABI revision at all, in which case
 * the caller diagnoses it rather than passing something else.
 */
typedef struct ArgSlot {
    bool ok;
    bool in_register;
    unsigned pieces;
    AggClass classes[2];
    unsigned registers[2];   /* register number within its class */
    size_t stack_offset;     /* bytes past the return address */
} ArgSlot;

/* Assign every argument in a list, in order, and return the total size of the
   outgoing argument area in bytes. `types` holds one type per argument and
   `slots` receives one filled slot per argument. */
size_t abi_assign_arguments(Type *const *types, size_t count, ArgSlot *slots);

#endif
