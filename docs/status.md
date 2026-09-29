# CC64 implementation status

Status date: 2026-09-28. The recorded `make check-release` run passed every
stage: 24 QEMU image cases and two linked target-library
cases, 8 Bochs conformance cases in both image forms, all
fourteen production units compiled on the target to byte-identical objects,
the relinked compiler image byte-identical, the 24 conformance
cases compiled, linked, and run by a compiler the target built, the fuzz and
property gate, the license and provenance audit, and a reproducible clean
build over 45 files with digest `1b7531ed3a4f06ecd025852f8219682ee663fcca79a8f3d5defbe777a4c40f3a`.

Bootstrap audit record: GCC 16.2.1, GNU Make 4.4.1, Python 3.14.7,
QEMU 11.1.1, Bochs 3.1, Git 2.55.0. `make clean && make check` and
`make check-release` pass locally. The target matrix is measured with
compiler-produced images against MS-DOS64 revision
`13c3cedb05ad75592c17bf2006ba8617c8761a38`; Bochs runs the pinned raw and
`MZ64` cases, and `make check-release` covers the QEMU image cases, the linked
target-library cases, the self-host fixed point, the conformance corpus run by
a compiler the target built, the Bochs corpus subset, the property gate, the
performance measurement, and the license inventory.

One target change is active and is recorded in
`docs/provenance-ledger.md`: MS-DOS64 `edit` revision `f36e84c` (over `8b989fb`)
keeps the shell's command-tail pointer in a callee-saved register and records
the invocation name in the child's first file control block, so a started
program receives both its name and its arguments. Integration against that
branch is deliberate and prints the edit revision, its ancestry from the pinned
reference, and the file it changes; `tools/target_revision.py` is the single
place that decision lives. Strict release mode refuses an edit-branch run only
when a revision override is also requested, because the override exists to
name a different target; an edit-branch run on its own is deliberate and is
reported in the run's output rather than hidden.

| Milestone | State | Evidence |
|---|---|---|
| M0 contract, provenance, skeleton | complete | versioned ABI, object, `COM`, and `MZ64` specifications; arena, source manager, diagnostics, driver, unit and audit harness; an independent clean-room review of every tracked file recorded in `docs/source-origin-audit.md`, and every checklist item in `docs/clean-room-checklist.md` answered |
| M1 lexer and preprocessor | complete | phase-aware lexer, comments and splices, literals, keywords, object/function/variadic macros, hidesets, conditionals, includes, line and end handling, predefined macros, bounded expansion, and the boundary matrix: self-reference, mutual reference, a chain deeper than the expansion bound, a missing include, a real header chain resolved from disk, a header that includes itself reported as a cycle, `__LINE__` on the line the token is written, a token count past the limit, and the rule that a run that fails publishes no output |
| M2 parser, types, semantics | complete | typed AST, the C17-subset declarations and declarators, scopes and linkage, structs, unions, enums, expressions, statements, conversions, initializers, `static_assert`, `__func__`, `_Alignof`, a stable diagnostic for every deferred construct, a conversion group covering the usual arithmetic conversions, decay, the comma operator's sequence, layout for ten aggregate shapes, block scoping, and constant-expression folding, and the semantic group's positive, deferred, and negative cases |
| M3 IR and x86-64 backend | complete | typed linear IR, stack-temporary lowering, an x86-64 encoder with a verified condition-code table, the frame and call model, multi-dimensional aggregate lowering, the `CC64O` writer, relocations, an independent object inspector, a project-owned machine-code decoder with a mode that decodes a section or one symbol, and deterministic object tests. The ABI, register, and stack invariants are covered by a unit group that reads the compiler's own emitted code: the frame prologue and epilogue, the frame size the code reserves against the one the lowerer recorded, the argument register order, the callee-saved rule, and the target service boundary |
| M4 linker, loader image, runtime | complete | independent `CC64O` validation, section and symbol merge, relocation application with overflow checks, deterministic raw `.COM`, the target entry and exit path, freestanding target headers, the target library, and QEMU and Bochs evidence for both image forms |
| M5 language and MS-DOS64 compatibility | complete | pointers, arrays, nested and multi-dimensional initializers, structs, unions, enums, switch and short-circuit control flow, increments and compound assignment, stack arguments, scalar and aggregate copies, binary32 and binary64 arithmetic, variadic calls, the target runtime service set, formatted output, floating arithmetic and comparison, and the conformance corpus running on both emulators. Bochs runs a named subset of the corpus chosen for what the second emulator has to agree about, because it is far slower than the other one, and a named case that the corpus drops is a failure rather than a silently smaller run |
| M6 `MZ64`, diagnostics, hardening | complete | `MZ64` header and table emission, image-relative data fixups, BSS sizing, full-file and section CRC validation, bounded relocation and object records, malformed-image rejection, deterministic raw and `MZ64` links, load-bias checks at five biases and at two that must be refused, every header field the loader reads mutated and required to be rejected, every relocation field driven out of range, extreme constants, and deep nesting in three constructs. Diagnostic recovery is covered: an error marks the declaration wrong, the parse continues, and a case shows three independent errors reported in one run and the declaration that follows an error still parsed |
| M7 self-hosting | complete | all fourteen production translation units compile on the target to objects byte-identical to the bootstrap compiler's, the self-hosted linker relinks them into a compiler image byte-identical to the bootstrap image, a target-built compiler reproduces one project's object, image, and exit code, and the whole conformance corpus is compiled, linked, and run by a compiler the target built with every object compared byte for byte |
| M8 release quality | complete | path-independent clean-build hash comparison, deterministic malformed source, object, and image smoke, the automated provenance and source-origin audit, QEMU and Bochs target evidence, and the aggregate release gate, a license inventory the audit enforces against every tracked file, and a performance gate that measures the clean build twice, each production unit, and both image forms against the budgets the contracts state |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.

## Release gate

`make check-release` currently runs and passes the host, object, raw and
`MZ64`, QEMU, Bochs, deterministic source/object/image smoke, reproducibility,
self-host, provenance, and target-library checks. `make check-release-strict`
additionally requires a clean source tree and refuses to accept a missing
emulator as a pass.

## What the self-host evidence does and does not claim

A target-built compiler reproducing every corpus object byte for byte, and
running every case to the same exit code and output, is strong evidence that
the compiler is a fixed point on every construct the corpus uses. It is not a
claim that the corpus is exhaustive: the corpus covers the constructs the
version 1 subset accepts, not every program expressible in it. A defect that
needs a construct no case uses is not excluded by that gate.

## Known limits of the version 1 contract

These are contract limits rather than open work, and each is diagnosed or
refused rather than approximated:

- Aggregate-by-value parameter passing and aggregate return values are not
  part of the version 1 ABI; a by-value aggregate parameter is diagnosed.
- Floating variadic arguments are not part of the version 1 variadic contract,
  and a floating conversion is not part of the target formatter's subset.
- The version 1 startup builds `argv` from the process control block and the
  command tail and passes a null `envp`, because the target supplies no
  environment block.
- The conformance corpus, the object gate, and the fuzz gate bound what they
  test. They are evidence for the recorded contract, not a proof of it.
