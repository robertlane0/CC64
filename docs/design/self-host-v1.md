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

A target-header compile probe of the complete compiler sources still reaches
the deferred aggregate-by-value parameter/return ABI (for example the `Token`
value parameter in `frontend.h`) and needs a target `stdio`/`stdlib` surface
for source loading, allocation, diagnostics, and object writing. The current
probe is diagnostic evidence, not a release gate; no host compiler is used to
translate or bless target output. These dependencies are recorded as open M7
work rather than hidden behind the stage1 result.
