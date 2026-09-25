# CC64 implementation status

Status date: 2026-09-24.

Bootstrap audit record: GCC 16.2.1, GNU Make 4.4.1, Python 3.14.7,
QEMU 11.1.1, Bochs 3.1, Git 2.55.0; validation source revision `738e6a7`.
`make clean && make check` and `make check-release` pass locally. The target
matrix is measured with compiler-produced images against MS-DOS64 revision
`13c3cedb05ad75592c17bf2006ba8617c8761a38`; Bochs now runs the pinned raw
image case successfully.

| Milestone | State | Evidence |
|---|---|---|
| M0 contract, provenance, skeleton | partial | versioned ABI/object/COM/MZ64 specs; arena, source manager, diagnostics, driver, unit and audit harness; human clean-room review record remains open |
| M1 lexer and preprocessor | partial | phase-aware lexer, comments/splices, literals, keywords, object/function/variadic macros, hidesets, conditionals, includes, line/EOF handling, predefined macros, bounded expansion, and frontend/driver tests; full boundary/resource matrix remains open |
| M2 parser, types, semantics | partial | typed AST, C17-subset declarations/declarators, scopes/linkage, structs/unions/enums, expressions/statements, conversions, initializers, and deferred-feature diagnostics; conformance coverage remains open |
| M3 IR and x86-64 backend | partial | typed linear IR, stack-temporary lowering, x86-64 encoder, frame/call model, CC64O writer, relocations, independent object inspector, and deterministic object tests; complete ABI/object invariant coverage remains open |
| M4 linker, loader image, runtime | partial | independent CC64O validation, section/symbol merge, PC-relative relocation checks, deterministic raw `.COM`, target entry/exit trampoline, freestanding target headers, and QEMU/Bochs smoke; full runtime I/O coverage remains open |
| M5 language and MS-DOS64 compatibility | partial | pointers, arrays, nested initializers, structs/unions, enums, switch/short-circuit control flow, increments/compound assignment, stack arguments, scalar/aggregate copies, basic binary32/binary64, target runtime service stubs, seven QEMU cases, and one Bochs raw case; full corpus/runtime/file/process coverage remains open |
| M6 `MZ64`, diagnostics, hardening | partial | MZ64 header/table emission, image-relative data fixups, BSS sizing, full-file/section CRC validation, bounded relocation/object records, malformed-image rejection, deterministic raw/MZ links, and QEMU data-pointer execution; broad negative/load-bias and Bochs MZ64 coverage remain open |
| M7 self-hosting | partial | CC64-built `stage1` compiles, links as MZ64, is checked twice, and returns `Exit 51` under QEMU; the complete compiler source is not target-self-hosted |
| M8 release quality | partial | path-independent clean-build hash comparison, deterministic malformed source/object/image smoke, automated provenance/source-origin audit, QEMU/Bochs target evidence, and aggregate release gate run; human review and full self-hosting remain open |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.

## Release gate

`make check-release` currently runs and passes the host, object, raw/MZ64,
QEMU, Bochs, deterministic source/object/image smoke, reproducibility, and
provenance checks. The measured clean bootstrap build manifest digest is
`fa70dab34fc35cbb2a3b98dc5c547def7be7feed9f070d72759312c10d23b0de`
across 37 generated files and deterministic target artifacts.

This is a validation checkpoint, not a releasable M7/M8 claim. Full
self-hosting, target-hosted compiler I/O and formatting, complete argv/envp
construction, aggregate-by-value parameters, and the full QEMU/Bochs
conformance matrix remain open. A release must record those limitations
rather than treating a successful local aggregate command as completion.
`make check-release-strict` now enforces the clean-tree and emulator
requirements, but it does not convert partial language/self-hosting coverage
into release approval.
