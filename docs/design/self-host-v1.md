# Bootstrap stage validation

The first target-side bootstrap gate uses `src/selfhost/stage1.c`, an original
small C program with its own deterministic parser and result checksum. The
host bootstrap compiler emits its `CC64O` and `MZ64` image, and the target
loader runs the resulting stage under QEMU. `make self-host` builds and runs
the stage twice, compares the two images, and records the expected `Exit 51`
result. The adjacent target checkout is revision-pinned by the test scripts;
when an emulator is unavailable, the local gate reports a skip, while strict
release mode rejects that condition.

This gate is intentionally separate from host `make`: it proves that target
objects, MZ64 relocation, startup, integer calls, local aggregate/string
initialization, and target execution work together without a target assembler
or linker. It is not full compiler self-hosting. A later self-hosting
milestone will replace the small stage with the complete compiler source
after its remaining target-library and language dependencies are implemented.

## Current blockers

The target-header profile now compiles every production compiler translation
unit individually through `make selfhost-probe`. The remaining work is to
supply the target runtime implementation, link the complete object set, and
run the resulting target compiler. The target library must provide the
compiler's file, allocation, string, and formatting operations without
including a host libc. A target-built compiler must then reproduce the
host compiler's objects and diagnostics on the conformance corpus. These
dependencies are recorded as open M7 work rather than hidden behind the
stage1 result.
