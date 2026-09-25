# CC64 implementation status

Status date: 2026-09-24.

Bootstrap audit record: GCC 16.2.1, GNU Make 4.4.1, Python 3.14.0;
`make clean && make check` passes. The target emulator matrix is not yet a
CC64-generated-image result.

| Milestone | State | Evidence |
|---|---|---|
| M0 contract, provenance, skeleton | complete | versioned ABI/object/COM/MZ64 specs; arena, source manager, diagnostics, driver, unit and audit harness |
| M1 lexer and preprocessor | complete | phase-aware lexer, comments/splices, literals, keywords, object/function/variadic macros, hidesets, conditionals, includes, line/EOF handling, predefined macros, bounded expansion, and frontend/driver tests |
| M2 parser, types, semantics | not started | — |
| M3 IR and x86-64 backend | not started | — |
| M4 linker, loader image, runtime | not started | — |
| M5 language and MS-DOS64 compatibility | not started | — |
| M6 `MZ64`, diagnostics, hardening | not started | — |
| M7 self-hosting | not started | — |
| M8 release quality | not started | — |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.
