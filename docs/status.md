# CC64 implementation status

Status date: 2026-09-24.

Bootstrap audit record: GCC 16.2.1, GNU Make 4.4.1, Python 3.14.0;
`make clean && make check` passes. The target emulator matrix is not yet a
CC64-generated-image result.

| Milestone | State | Evidence |
|---|---|---|
| M0 contract, provenance, skeleton | complete | versioned ABI/object/COM/MZ64 specs; arena, source manager, diagnostics, driver, unit and audit harness |
| M1 lexer and preprocessor | complete | phase-aware lexer, comments/splices, literals, keywords, object/function/variadic macros, hidesets, conditionals, includes, line/EOF handling, predefined macros, bounded expansion, and frontend/driver tests |
| M2 parser, types, semantics | complete | typed AST, C17-subset declarations/declarators, scopes/linkage, structs/unions/enums, expressions/statements, conversions, initializers, and deferred-feature diagnostics with positive/negative tests |
| M3 IR and x86-64 backend | complete | typed linear IR, stack-temporary lowering, x86-64 encoder, frame/call model, CC64O writer, relocations, independent object inspector, deterministic object tests |
| M4 linker, loader image, runtime | complete | independent CC64O validation, section/symbol merge, PC-relative relocation checks, deterministic raw `.COM`, target entry/exit trampoline, freestanding target headers, and QEMU compiler-produced-image run returning exit 7 |
| M5 language and MS-DOS64 compatibility | complete | pointers, arrays, nested initializers, structs/unions, enums, switch/short-circuit control flow, increments/compound assignment, stack arguments, scalar/aggregate copies, basic binary32/binary64, target runtime service stubs, and five QEMU raw/MZ64 image cases |
| M6 `MZ64`, diagnostics, hardening | in progress | MZ64 header/table emission and target data-pointer execution are working; independent malformed-input and release gates remain |
| M7 self-hosting | not started | — |
| M8 release quality | not started | — |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.
