# CC64

CC64 is an original C17 compiler and native toolchain for 64-bit MS-DOS64.
It emits its own `CC64O` objects, links raw `.COM` and relocatable `MZ64`
images, and does not invoke a target assembler or linker.

## Bootstrap build

A host C17 compiler is used only to build the compiler driver:

```sh
make
make check
```

The aggregate validation checkpoint is `make check-release`; it also runs
malformed-input smoke, target-profile self-host probing, and clean-build
reproducibility checks. Use
`make check-release-strict` when missing QEMU, target, or Bochs evidence must
fail instead of being reported as skipped. The current status, including the
intentionally partial self-hosting and conformance matrix, is recorded in
`docs/status.md` and `docs/release-matrix.md`.

No target program is built by the host compiler. The MS-DOS64 checkout, if
present beside this repository, is an interface reference and test target; it
is not part of the CC64 source or normal build.

## Current contracts

- `docs/specs/abi-v1.md`
- `docs/specs/cc64o-v1.md`
- `docs/specs/com-v1.md`
- `docs/specs/mz64-v1.md`

## Milestones

The implementation sequence and acceptance gates are defined in `AGENTS.md`.
`docs/status.md` records measured completion. Unsupported language features
are diagnosed rather than silently accepted.
