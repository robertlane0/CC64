# CC64 implementation status

Status date: 2026-09-25.

Bootstrap audit record: GCC 16.2.1, GNU Make 4.4.1, Python 3.14.7,
QEMU 11.1.1, Bochs 3.1, Git 2.55.0; validation source revision `4403831`.
`make clean && make check` and `make check-release` pass locally. The target
matrix is measured with compiler-produced images against MS-DOS64 revision
`13c3cedb05ad75592c17bf2006ba8617c8761a38`; Bochs runs the pinned raw and
`MZ64` cases, and `make check-release` covers twenty-one QEMU image cases, the
linked target-library case, and the self-host fixed point.

One target change is active and is recorded in
`docs/provenance-ledger.md`: MS-DOS64 `edit` revision `f36e84c` (over `8b989fb`)
keeps the shell's command-tail pointer in a callee-saved register and records
the invocation name in the child's first file control block, so a started
program receives both its name and its arguments. Integration against that
branch is deliberate and prints the edit revision, its ancestry from the pinned
reference, and the file it changes; `tools/target_revision.py` is the single
place that decision lives, and strict release mode refuses an edit-branch run.

| Milestone | State | Evidence |
|---|---|---|
| M0 contract, provenance, skeleton | partial | versioned ABI/object/COM/MZ64 specs; arena, source manager, diagnostics, driver, unit and audit harness; human clean-room review record remains open |
| M1 lexer and preprocessor | partial | phase-aware lexer, comments/splices, literals, keywords, object/function/variadic macros, hidesets, conditionals, includes, line/EOF handling, predefined macros, bounded expansion, and frontend/driver tests; full boundary/resource matrix remains open |
| M2 parser, types, semantics | partial | typed AST, C17-subset declarations/declarators, scopes/linkage, structs/unions/enums, expressions/statements, conversions, initializers, and deferred-feature diagnostics; conformance coverage remains open |
| M3 IR and x86-64 backend | partial | typed linear IR, stack-temporary lowering, x86-64 encoder with a verified condition-code table, frame/call model, multi-dimensional aggregate lowering, CC64O writer, relocations, an independent object inspector, and deterministic object tests; complete ABI/object invariant coverage remains open |
| M4 linker, loader image, runtime | partial | independent CC64O validation, section/symbol merge, PC-relative relocation checks, deterministic raw `.COM`, target entry/exit trampoline, freestanding target headers, and QEMU/Bochs smoke; full runtime I/O coverage remains open |
| M5 language and MS-DOS64 compatibility | partial | pointers, arrays, nested and multi-dimensional initializers, structs/unions, enums, switch/short-circuit control flow, increments/compound assignment, stack arguments, scalar/aggregate copies, basic binary32/binary64, the target runtime service set, formatted output, relational operators and the conditional operator, twenty-one QEMU cases, and Bochs raw and `MZ64` cases; a conformance corpus and full runtime/file/process coverage remain open |
| M6 `MZ64`, diagnostics, hardening | partial | MZ64 header/table emission, image-relative data fixups, BSS sizing, full-file/section CRC validation, bounded relocation/object records, malformed-image rejection, deterministic raw/MZ links, and QEMU data-pointer execution; broad negative/load-bias and Bochs MZ64 coverage remain open |
| M7 self-hosting | partial | all 14 production translation units and the 6-file target C library compile with the target header profile; the complete compiler links with no unresolved symbols into a roughly 400 KB `MZ64` image, boots on the target, compiles a project-authored source with its own front end, links the result with its own linker, and reproduces the bootstrap compiler's object and image byte for byte, with the linked image executing and returning its expected code; the fixed point is demonstrated for one translation unit, so compiling the compiler's own sources on the target, which needs the include tree on the volume, and comparing all 14 stage-two objects still has to run |
| M8 release quality | partial | path-independent clean-build hash comparison, deterministic malformed source/object/image smoke, automated provenance/source-origin audit, QEMU/Bochs target evidence, and aggregate release gate run; human review and full self-hosting remain open |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.

## Release gate

`make check-release` currently runs and passes the host, object, raw/MZ64,
QEMU, Bochs, deterministic source/object/image smoke, reproducibility, and
provenance checks. The measured clean bootstrap build manifest digest is
`b85252e35eb7c22539231f975a041a3776c0737ed6b9c119abae973264e24321`
across 42 generated files and deterministic target artifacts.

This is a validation checkpoint, not a releasable M7/M8 claim. Full
self-hosting, target-hosted compiler I/O and formatting, complete argv/envp
construction, aggregate-by-value parameters, and the full QEMU/Bochs
conformance matrix remain open. A release must record those limitations
rather than treating a successful local aggregate command as completion.
`make check-release-strict` now enforces the clean-tree and emulator
requirements, but it does not convert partial language/self-hosting coverage
into release approval.
