# CC64 implementation status

Status date: 2026-09-29. The recorded `make check-release` run passed every
stage: 35 QEMU image cases and four linked target-library cases, 8 Bochs
conformance cases in both image forms, all fifteen production units compiled
on the target to byte-identical objects, the relinked compiler image
byte-identical, the 35 conformance cases compiled, linked, and run by a
compiler the target built, the target heap measured at eleven mebibytes, the
date a compiler-produced program reads back from the target, the response-file
gate, the seven unit groups, the linker-rejection and
image-compatibility gates, the property gate, the license and provenance
audit, and a reproducible clean build over 53 files with digest
`4ef239cf9ae034bf59b79bb97b5da6df437da0ffcab5e8260e45202b49930ac7`.

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

One target change followed from that. `make full` did not build on the `edit`
branch: the two shell commits that precede the heap change grew the kernel to
131,104 bytes, one sector past the 256 the disk layout allowed. It was unrelated
to the heap, which adds no bytes to the kernel. The kernel slot is now 512
sectors, which is the slot doubled (D-135), and `make full` builds again at
131,136 bytes. Doubling the slot is not one line, and the two consequences are
worth naming because both were found by running the target rather than by
reading it:

- The kernel extent `[16, 528)` covered the volume at LBA 512 and the scratch
  block at 500..511, so the volume moved to 1024 and scratch to 600 and
  700..711, into the band the move freed. The ordering the layout relies on —
  boot, then kernel, then ATA scratch, then FS scratch, then volume — still
  holds, and `check-layout` and the target's own layout test both pass.
- The loader stages the whole kernel in real mode before copying it to 1 MiB,
  and its buffer sat at `0x70000` under the BIOS INT 13h stack at `0x90000` and
  the VGA window. That capped the slot at 256 sectors, so raising the slot
  without moving the buffer copied a quarter-mebibyte of scratch and volume
  into the kernel image: five unrelated selftests failed at once, which is what
  a truncated kernel image looks like. The buffer is now at `0x10000`, where
  512 sectors reach `0x50000`.

The target's own suite is now 94 pass, 1 fail with the destructive tests
running, against 88 pass, 1 fail with six skipped before. The one failure was
the RTC date/time test, and it is now fixed; see below.

The volume move broke nine CC64 harnesses and both FAT12 tools, which each
carried the volume's address as a constant and so wrote and read sectors the
volume did not occupy (D-134). They now read the geometry from the boot sector
the target's own stamper wrote, which follows whichever target revision is
checked out rather than the one a constant was written against.

## The clock kept a month, and the compiler could not read the date

The target's suite had one failure throughout: the RTC date/time test, which
failed identically on the unmodified reference and had therefore never passed.
It is now fixed, and finding why turned up a second defect on this side.

**The target.** `rtc_set_date64` wrote the year, then the month, then the day.
The month write lands on whatever day the clock is still holding, so moving to
a month with fewer days than the current day names a date that never existed —
the 29th of a 28-day month — and the clock then has to resolve it. Measured with
the clock holding the 29th: a request for 2001-02-28 was answered as
**2001-03-28**, the month carried forward and the day lost. Writing the day as
1 first makes every intermediate date real, because the 1st exists in every
month. The suite goes from 88 pass, 1 fail, 6 skipped to **89 pass, 0 fail**,
and from 94 pass, 1 fail to **95 pass, 0 fail** with the destructive tests
running, on QEMU and on Bochs. Recorded as D-137.

The fix is nine lines in `src/kernel/time64.asm` and the target's `edit` branch
at `11b3cbb`. It is deliberately not reproduced by a CC64-side test: whether the
clock's update tick lands between the guest's writes decides what an impossible
date normalises to, so an external test of it would be testing the emulator's
timing rather than either side. The target's own suite is the test.

**The compiler.** With the target correct, a program still could not read the
date: the runtime thunk that folds the two registers `AH=2Ah` returns shifted
them instead of joining them, so it returned the year shifted down and the
month shifted up and dropped the day. A program asking the target the time of
day was told **2334-7-23**. The same shape was wrong for the time thunk, and
the two were written identically, so both were corrected to move the register
that already holds its fields into place (D-136). Each half is now taken 32
bits wide, because the service leaves the rest of the register to the caller
and the shift form was returning those bits too.

`make target-rtc` pins the compiler's half: the harness sets a date with the
target's own command and a compiler-produced program reads it back. It fails
with the old thunk (`a program reads 2334-7-23`) and passes with the new one
(`2001-02-28`), so the defect is reproduced and the fix is measured.

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

## A large program compiled with this compiler

`tools/edit_build.py` builds a 24,461-line C program — a text editor, 111
files — with CC64, without modifying a line of it, and places the result on a
target volume. All 38 translation units compile and the objects link with the
target library into a 590,241-byte load-biased image, which the target loads
and starts: `Loaded, pid 1` from `build/dos64-lean.img` under QEMU.

The program used to exit 1 with no message. Its arena and gap buffers reserve
address space they do not commit — two scratch arenas of 512 MiB each, two
interface arenas of 128 MiB each, and a 4 GiB document buffer, about 5.25 GiB in
total — and a reservation the target cannot honour is fatal, so
`edit_scratch_init` failed and the start-up path returned 1 silently.

With permission to change c-edit's architecture, on the branch `ms-dos64`, the
fix is 36 lines in three files and changes no behaviour on a host. A capacity is
a ceiling, not a promise, so `edit_arena_init` now falls back to a fixed
`EDIT_ARENA_TARGET_BYTES` when the platform cannot reserve the capacity asked
for, and `edit_gap_init` does the same with `GAP_TARGET_RESERVE`. Both are one
mebibyte. The figure is fixed rather than "whatever is left" on purpose: a
halving fallback was tried first and it is wrong, because the first arena to
ask takes the whole heap — measured, an 8 MiB reservation — and every later
allocation is starved, so the editor failed a step later instead of starting. A
host with the address space reserves the full cap and is unaffected, which is
why c-edit's own suite is unchanged: `make test`, `make release-test` and
`make asan` all pass.

The editor now starts, initialises every subsystem, and draws its first screen
on the target: a menubar reading `(F) (E) (V) Close Editor(H)`, a ruler, a
sidebar, and a status line reporting `[LF] [UTF-8] 1:1 [Untitled-1.txt]`.
`python3 tools/edit_build.py --run` reproduces the whole claim in one command:
it compiles the 38 units, links the image, boots it, and reports
`Loaded, pid 1` and `first screen drawn`, failing if either is absent.

Interactive input does not reach it, and the reason is in the target rather
than in the program or the compiler. The target's console-input-status service
(`INT 21h` `AH=0Bh`) checks the PS/2 keyboard buffer and the kernel keyboard
queue, and both emulators deliver a test's keystrokes over the serial line,
which that service does not inspect. Measured with a program that polls
`cc64_console_ready()` 200 times while a byte is waiting: the byte is visible
at the shell prompt behind it, and the service never reports it ready. The
editor therefore blocks in `edit_tty_read` after its first frame. Making that
service see the serial line is a target change, and it is not required to make
the compiler operational, so it is reported here rather than made.

What passes: every unit group, the host integration group, the 35 conformance
cases on QEMU in both image forms, the four linked target-library cases, the
target's own arithmetic suite, every function the target headers declare, the
8 Bochs cases, the self-host image built and run twice with both runs
identical, the self-host stage, the self-host probe, the self-host corpus, the
response-file gate, the target-heap gate, the target-clock gate, the target
library build, the linker rejections, the image compatibility matrix, the
property gate, the performance measurement, the reproducible build, and the
license and provenance audit. The target's own suite is 95 pass, 0 fail with
its destructive tests running.

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
