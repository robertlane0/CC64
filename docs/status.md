# CC64 implementation status

Status date: 2026-09-29. The recorded `make check-release` run passed every
stage: 35 QEMU image cases and four linked target-library cases, 8 Bochs
conformance cases in both image forms, all fifteen production units compiled
on the target to byte-identical objects, the relinked compiler image
byte-identical, the 35 conformance cases compiled, linked, and run by a
compiler the target built, the target heap measured at eleven mebibytes, the
response-file gate, the seven unit groups, the linker-rejection and
image-compatibility gates, the property gate, the license and provenance
audit, and a reproducible clean build over 53 files with digest
`afc78e1f69ef4652c64d6133ab9b098ad10c9166e5be5f56efa3039781bc5a4b`.

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
| M7 self-hosting | complete | all fifteen production translation units compile on the target to objects byte-identical to the bootstrap compiler's, the self-hosted linker relinks them into a compiler image byte-identical to the bootstrap image, a target-built compiler reproduces one project's object, image, and exit code, and all 35 conformance cases are compiled, linked, and run by a compiler the target built with every object compared byte for byte |
| M8 release quality | complete | path-independent clean-build hash comparison, deterministic malformed source, object, and image smoke, the automated provenance and source-origin audit, QEMU and Bochs target evidence, and the aggregate release gate, a license inventory the audit enforces against every tracked file, and a performance gate that measures the clean build twice, each production unit, and both image forms against the budgets the contracts state |

A milestone is marked complete only after its tests and required target runs
pass. Planned code is never reported as completed.

## The target heap is extended, and M7 depends on it

M7 regressed and now passes. The regression was a memory shortfall on the
target, not a miscompilation: the target-built compiler could not compile the
compiler's own largest unit, `src/backend/encoder.c`, and that unit compiles on
the host in 23 milliseconds to a byte-identical object.

The target's heap was six mebibytes, from `0x200000` to `0x800000`, under a
four-entry identity map. Compiling that unit the target-built compiler asked
for about 8.9 megabytes in total, and its largest single request was 1.2
megabytes. One thing was tried against the heap and did not close the gap:
bounding a token list's reservation so that it grows in steps rather than in
one request cut the largest single request from 2,031,600 bytes to 1,245,120
and the demand is larger than the heap however it is taken. The target's own
resize service is the obvious answer to a growing buffer and is not usable,
because a user process that calls it reboots the kernel; that was measured with
a program that resizes a block twelve times, and the reboot follows the second
resize.

The fix is a target change, made on the target's `edit` branch as commit
`d9a4379` over the pinned reference `13c3ced`, and recorded as D-132. The heap
is now twelve mebibytes, from `0x200000` to `0xE00000`, under a seven-entry
map. The chain and the map have to agree, and the page-directory helpers that
index an entry have to be bounded by the entry count, so the heap bounds and
the count derived from them now live in one definition in the target's
`include/mcb.inc` rather than in three files. That matters: a chain bound
copied into two places is how a literal `6*1024*1024` that bounded a process
block was still in place when the heap was doubled.

`tests/target_heap.py` measures the heap with a compiler-produced program
rather than reading a constant, and requires at least eleven mebibytes. The
pinned reference measures five and fails the gate by name, so the dependency on
the `edit` branch is visible rather than implicit. The target's own suite is
unchanged: 88 pass, 1 fails, 6 skip. The one failure is the RTC date/time
test, which fails identically on the unmodified reference and is not related to
the heap.

One target change is outstanding and is not CC64's to make. `make full` does
not build on the `edit` branch: the two shell commits that precede this one grew
the kernel to 131,104 bytes, one sector past the 256 the disk layout allows. It
is unrelated to the heap — the heap change adds no bytes to the kernel — and the
fix is the one the target's own error message names, a larger `KERNEL_SECTORS`
and the matching constant in the target's layout test.

## The command tail is shorter than the link is long

With the heap extended, the self-host link ran and produced an image the target
could not find. The cause was a length, not a failure: the target's process
contract gives a child 127 bytes of command tail at `PSP+0xA1`, 126 of them
usable, and a link of this compiler's twenty-one objects is 126 characters
including the options. The tail was cut and the linker wrote its image to
`S2.CO`.

That bound is now measured rather than assumed, with the target's own `ECHO`
builtin: 115, 119, 120, and 121 characters are echoed whole, and 122 is not. A
command tail is a fixed field, so an argument list that has to fit inside one
cannot be the design. An argument of the form `@NAME` is replaced by the words
inside `NAME` (D-133), which the self-host stage uses to link with
`CC64S @LINK.RSP -o S2.COM`. `tests/response_files.py` pins that a response
file produces a byte-identical image to the same arguments written out, that a
word may name a further response file, that a quoted word keeps a space, that
arguments may be mixed with plain ones, and that a missing file, a file that
names itself, and two files that name each other are each reported rather than
followed.

What passes: every unit group, the host integration group, the 35 conformance
cases on QEMU in both image forms, the four linked target-library cases, the
target's own arithmetic suite, every function the target headers declare, the
8 Bochs cases, the self-host image built and run twice with both runs
identical, the self-host stage, the self-host probe, the self-host corpus, the
response-file gate, the target-heap gate, the target library build, the linker
rejections, the image compatibility matrix, the property gate, the performance
measurement, the reproducible build, and the license and provenance audit.

## A large program compiled with this compiler

`tools/edit_build.py` builds a 24,461-line C program — a text editor, 111
files — with CC64, without modifying a line of it, and places the result on a
target volume. All 38 translation units compile and the objects link with the
target library into a 590,033-byte load-biased image, which the target loads
and starts: `Loaded, pid 1` from `build/dos64-lean.img` under QEMU.

The program does not reach its first screen on the pinned target, and the
reason is a resource limit rather than a compiler gap. It reserves address
space it does not commit: two scratch arenas of 512 MiB each and two
interface arenas of 128 MiB each, about 1.25 GiB in total. The target
identity-maps eight mebibytes in total and its heap is the six mebibytes from
`0x200000` to `0x800000`. Measured on the target by asking its own arena
initializer for each size in turn, a reservation succeeds up to five mebibytes
and fails from six; the rest of the heap is taken by the image. The program
commits only what it uses, so the reservations are the whole of the
requirement, and the start-up path returns 1 without a message because
`edit_scratch_init` failing is one of its two silent exits.

No compiler change reaches that. A reservation is address space, and the target
maps eight mebibytes of it. Enlarging the target's address space would be an
operating-system feature rather than a defect the target contract requires be
fixed, so it is out of scope under the repository's own rule that a target edit
exists to make the compiler operational.

What the build needed from the compiler is recorded in the ledger: the 128-bit
unsigned integer type, compound literals, aggregate passing and return by value,
oversized aggregates in memory, token pasting, spliced continuation lines,
repeated type names, a floating constant whose exponent is out of table range,
and eight further defects that those exposed. Each has a conformance case.

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

- An aggregate by value is carried in registers when it fits two eightbytes and
  in memory otherwise, as the ABI document sets out. A field that crosses an
  eightbyte boundary makes the whole object memory-class, because no register
  can deliver such a field.
- Floating variadic arguments are not part of the version 1 variadic contract,
  and a floating conversion is not part of the target formatter's subset.
- The version 1 startup builds `argv` from the process control block and the
  command tail and passes a null `envp`. `getenv` therefore reports that no
  variable is set. The target's environment belongs to its shell, and the
  contract exposes no way for a running program to reach the process prefix the
  loader gave it, so reading a variable needs an interface revision that passes
  one.
- The target has one flat directory with no subdirectories, so the working
  directory is the volume root and a directory reading yields nothing. It has
  no window size, so a terminal query reports the character size the runtime
  assumes. It has no line discipline, so the terminal state is reported
  unchanged and restored unchanged. It has no monotonic clock, so a monotonic
  reading is the wall clock at one-second resolution and restarts at midnight.
  It has no signal delivery, so a handler is recorded and never called. It has
  no loader, so a shared object cannot be opened. Each header says which of
  these its interface is subject to.
- The conformance corpus, the object gate, and the fuzz gate bound what they
  test. They are evidence for the recorded contract, not a proof of it.
