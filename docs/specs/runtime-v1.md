# Target runtime, version 1

The raw-image startup trampoline is emitted by the CC64 linker at text offset
zero. It establishes the documented zero-argument initial call, preserves the
`main` return byte, invokes the target `AH=4Ch` boundary, and keeps a `ret`
path for a bare image. `src/runtime/startup.S` records the same contract for a
standalone runtime object; it is target source and is not assembled by the
host bootstrap.

The freestanding headers in `include/cc64/` define target-width integer types,
the DOS64 handle constants, and the allocation/I/O boundary. Runtime functions
are deliberately small wrappers over target services; no host system-call
header or host libc assumption is permitted.
