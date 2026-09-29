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

`make check-release` is the aggregate validation gate. It runs the unit,
integration, object, and ABI groups; the compiler-produced image matrices on
both emulators; the self-host fixed point; the conformance corpus compiled,
linked, and run by a compiler the target built; the malformed-input smoke; the
reproducibility check; the license and provenance audit; and the performance
measurement. A missing emulator is a failure there, not a skip, so the gate
cannot record a pass for a run that booted nothing. `make check-release-strict`
additionally requires a clean source tree.

The measured state of every milestone, and the limits the self-host evidence
does and does not claim, are recorded in `docs/status.md` and
`docs/release-matrix.md`.

No target program is built by the host compiler. The MS-DOS64 checkout, if
present beside this repository, is an interface reference and test target; it
is not part of the CC64 source or normal build.

## Contracts

- `docs/specs/abi-v1.md` — registers, stack, types, variadics, and the target
  service boundary
- `docs/specs/cc64o-v1.md` — the object format
- `docs/specs/com-v1.md` and `docs/specs/mz64-v1.md` — the two image forms
- `docs/specs/ir-v1.md` — the typed intermediate representation
- `docs/specs/runtime-v1.md` — startup, the service boundary, and the library
- `docs/specs/c-subset-v1.md` — the accepted language, and the identifier of
  the diagnostic for every deferred construct

## Provenance

Every non-obvious design decision has a row in `docs/provenance-ledger.md`
naming the reason it was made independently. `docs/license-policy.md` is the
license inventory, and `docs/audit.py` fails the build when a tracked file is
not covered by it, names an external compiler, or is a generated artifact.

## Milestones

The implementation sequence and acceptance gates are defined in `AGENTS.md`.
`docs/status.md` records measured completion. Unsupported language features are
diagnosed with their own identifier rather than silently accepted.
