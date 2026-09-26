# Target runtime, version 1

The raw-image startup trampoline is emitted by the CC64 linker at text offset
zero. It establishes a 16-byte-aligned call, constructs a bounded `argv` view
from the target PSP command tail, preserves the `main` return byte, invokes the
target `AH=4Ch` boundary, and keeps a `ret` path for a bare image. The target
checkout currently supplies the one-argument compatibility view; a later ABI
revision will add complete tokenization and environment construction.

The encoder emits small, CC64-owned service thunks for referenced
`cc64_putc`, `cc64_read`, `cc64_write`, `cc64_alloc`, `cc64_free`,
`cc64_open`, `cc64_close`, `cc64_create`, `cc64_lseek`, and `cc64_exit`
symbols. `src/runtime/startup.S` records the standalone target contract; it is
target source and is not assembled by the host bootstrap.

Every service thunk follows one rule, because the target's own convention does
not separate failure from success in the result register: a target service
reports a refusal by setting the carry flag and leaving a small positive code in
`RAX`, and such a code is a valid-looking handle, offset, or count. A thunk
returns the service value when the carry flag is clear and `-1` when it is set,
and the runtime library is written against that one rule. `cc64_alloc` is the
reason this matters most: a refused allocation reports the size of the largest
free block, which is a usable-looking address, so a pointer-returning service
that tested only for null would hand the caller a pointer into the middle of
the heap.

The freestanding headers in `include/cc64/` define target-width integer types,
the DOS64 handle constants, and the allocation/I/O boundary. Runtime paths
use only the documented target `INT 21h` services; no host system-call header
or host libc assumption is permitted.

A variadic function keeps the version 1 argument contract of the ABI document:
the six integer argument registers are spilled into a save area in its own
frame, the incoming stack argument area is copied into the slots that follow,
and `va_arg` walks the whole area eight bytes per slot. A call that passes more
than six unnamed arguments therefore works. The copy is bounded at eight stack
slots, and a slot the caller did not fill is copied but never read.

The target keeps thirteen file handles for a whole process. Every handle a
stream owns is released by `fclose`, readable or writable, because a retained
read handle is invisible to the program that leaked it and still occupies a
slot for the next open.

The freestanding headers in `include/cc64/` define target-width integer types,
the DOS64 handle constants, and the allocation/I/O boundary. Runtime paths
use only the documented target `INT 21h` services; no host system-call header
or host libc assumption is permitted.
