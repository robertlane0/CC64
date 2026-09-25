# Target runtime, version 1

The raw-image startup trampoline is emitted by the CC64 linker at text offset
zero. It establishes a 16-byte-aligned call, constructs a bounded `argv` view
from the target PSP command tail, preserves the `main` return byte, invokes the
target `AH=4Ch` boundary, and keeps a `ret` path for a bare image. The target
checkout currently supplies the one-argument compatibility view; a later ABI
revision will add complete tokenization and environment construction.

The encoder emits small, CC64-owned service thunks for referenced
`cc64_putc`, `cc64_read`, `cc64_write`, `cc64_alloc`, `cc64_free`,
`cc64_open`, `cc64_close`, and `cc64_exit` symbols. `src/runtime/startup.S`
records the standalone target contract; it is target source and is not
assembled by the host bootstrap.

The freestanding headers in `include/cc64/` define target-width integer types,
the DOS64 handle constants, and the allocation/I/O boundary. Runtime paths
use only the documented target `INT 21h` services; no host system-call header
or host libc assumption is permitted.
