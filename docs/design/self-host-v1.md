# Bootstrap stage validation

The first target-side bootstrap gate uses `src/selfhost/stage1.c`, an original
small C program with its own deterministic parser and result checksum. The
host bootstrap compiler emits its `CC64O` and `MZ64` image, and the target
loader runs the resulting stage under QEMU. `make self-host` checks the same
image twice and records the expected `Exit 51` result.

This gate is intentionally separate from host `make`: it proves that target
objects, MZ64 relocation, startup, integer calls, local aggregate/string
initialization, and target execution work together without a target assembler
or linker. A later self-hosting milestone will replace the small stage with
the complete compiler source after its remaining target-library dependencies
are implemented.
