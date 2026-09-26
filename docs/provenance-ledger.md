# CC64 provenance ledger

Last reviewed: 2026-09-24.

CC64 implementation code is authored for this repository from written contracts
and language/processor specifications. No implementation source, generated
table, machine-code fragment, test corpus, or build artifact from another
compiler is copied, translated, decompiled, or mechanically adapted.

## Permitted references

| Input | Use | Boundary |
|---|---|---|
| ISO/IEC 9899 C17 | token, declaration, expression, and statement semantics | behavioral requirements only; no implementation text |
| Intel SDM and AMD64 Architecture manuals | instruction encoding, flags, ABI-independent machine behavior | no copied code or generated encoding tables |
| System V AMD64 ABI specification | integer register assignment and stack alignment | specification only |
| `MS-DOS64/docs/*.md` observable contracts | PSP size, `INT 21h` interface, loader behavior, and emulator commands | interface facts only; no target source imported |
| Project-authored tests in `tests/` | expected CC64 behavior | original fixtures and assertions |

The nested `MS-DOS64/` directory is ignored and is neither a dependency nor a
source input to the normal CC64 build.

## Target changes on the edit branch

AGENTS.md section 1 permits a change in the `MS-DOS64` repository only on a
branch named `edit`, only to make the compiler operational, and only when it is
recorded here. `main` and the pinned revision stay unmodified so any build can
still be compared against the reference target.

| Edit revision | Reason and observed defect | Observable effect |
|---|---|---|
| `8b989fb` (branch `edit`, file `src/kernel/shell64.asm`) | CC64's startup contract requires a program to receive the text the shell was given after the program name, because `argc` and `argv` are built from it. A program started as `NAME ARG ARG` received an empty tail, and `argc` was always zero. The shell's program-execution path held the tail pointer in `R9` across the two calls that stage the image; the allocator uses `R9` as a scratch register, so the pointer handed to the spawn call was whatever the allocator left behind, and the child recorded a length measured from unrelated kernel memory. CC64 reproduced the defect with an original test (`C64H`, a program that prints its own argument vector) and located it by reading the length byte the target actually wrote. The change moves the tail pointer into a callee-saved register and restores it on every exit; it is minimal, carries nothing from CC64 into the target, and no target code was moved into CC64. | Before: `C64H` under the pinned revision receives `argc=0` and an empty first argument. After: it receives `argc=2` with `AA` and `BB`, and every image that parses arguments works. The pinned revision remains in the repository and is what the unmodified-target comparison uses. |

## Design decisions

| ID | Decision | Independent rationale |
|---|---|---|
| D-001 | Flat target and 16-byte stack, no red zone | target shares stacks with faults; deterministic interrupt-safe frame |
| D-002 | Project-owned `CC64O` tables and relocations | permits strict validation and deterministic target-only linking |
| D-003 | Stack-slot temporaries at `-O0` | simple frame invariants precede optimization |
| D-004 | `MZ64` uses explicit image-relative fixups | raw images cannot represent load-biased data addresses |
| D-005 | `int32_t` is the first target `int`/pointer-sized ABI | written target contract; avoids inheriting host LP64 |
| D-006 | Typed linear IR before machine selection | backend never consumes parser nodes |
| D-007 | Limits reject ambiguous or unbounded translation | deterministic failure for hostile input |
| D-008 | Preprocessing uses explicit hidesets and bounded expansion depth | prevents recursive macro runaway while retaining blue-painted tokens |
| D-009 | Failed preprocessing never publishes its temporary output | diagnostics cannot be mistaken for a successful translation |
| D-010 | Typed AST nodes carry explicit C types and source locations | later lowering cannot depend on parser token layout |
| D-011 | `-O0` uses balanced stack temporaries and explicit frame slots | makes ABI and call behavior inspectable before optimization |
| D-012 | Linker emits a target-owned entry/exit trampoline for raw COM | keeps startup independent of host CRT while preserving `main`'s return byte |
| D-013 | Target service functions are emitted by the CC64 encoder only when referenced | keeps the freestanding boundary in project-owned machine selection and avoids importing a target runtime |
| D-014 | `MZ64` data fixups use image-relative destinations and addends | the target loader supplies the load bias, so the same payload remains position-independent |
| D-015 | The startup stub exposes a bounded one-argument argv view from the target PSP | this is the minimum target interface needed by the first hosted compatibility gate; richer tokenization is recorded for the next ABI revision |
| D-016 | A separate stage1 smoke program gates target bootstrap execution | isolates loader, relocation, startup, and backend evidence from the larger self-hosting claim |
| D-017 | MZ64 relocation tables retain only image-relative data fixups | absolute/load-biased values are rejected instead of being silently made non-portable |
| D-018 | Release fuzzing uses a fixed PRNG seed and temporary inputs | malformed-input smoke remains reproducible without checked-in adversarial artifacts |
| D-019 | Reproducibility compares a content manifest of two path-independent clean exports, including compiler-produced target artifacts | detects nondeterministic object, archive, dependency, executable, or target-image output rather than checking one file only |
| D-020 | Target integration pins the adjacent MS-DOS64 revision and checks volume state before/after execution | prevents a passing local image from being attributed to an unpinned or modified loader or a dirty volume |
| D-021 | `src/runtime` is excluded from the host bootstrap object list | target runtime definitions are emitted for target builds rather than linked into the host driver |
| D-022 | A tagged aggregate definition completes the existing local tag object | preserves C typedef/forward-declaration identity for self-hosting and ordinary C clients |
| D-023 | `void *` compares compatibly with an object pointer | implements the C null/general-pointer comparison rule without inheriting host ABI behavior |
| D-024 | Image inspection rejects empty/oversized raw payloads and negative or out-of-range MZ64 fixups | keeps independent validation aligned with the pinned image contract and makes malformed-input tests meaningful |
| D-025 | Target service thunks preserve path and exit arguments in their target registers | keeps the encoder-owned runtime boundary faithful to the documented DOS register contract |
| D-026 | Strict release mode requires a clean source tree and non-skipped emulator evidence | prevents a local aggregate from certifying an unclean or incomplete release |
| D-027 | Independent object readers and the linker reject overlapping ranges, uncovered bytes, undersized executable alignment, and out-of-range symbol sizes | enforces the version-1 table layout before relocations or output are trusted |
| D-028 | Backend symbol ordering compares every serialized tie-breaker field | prevents nondeterministic ordering of otherwise equal-prefix symbol records |
| D-029 | Repeated encoder-emitted target service definitions are coalesced by the linker | permits multiple translation units to reference the same project-owned thunk without duplicate-symbol failures |
| D-030 | Bochs validation uses the pinned target bochsrc template, a term display, and a dedicated serial PTY | exercises the target's real serial-input shell path without importing emulator or OS implementation code |
| D-031 | Target compilation uses a dedicated standard-header shadow directory | keeps target translation units free of accidental host libc headers while preserving the compiler's source spellings |
| D-032 | Production translation units are probed individually with the target header profile before linking | makes self-host coverage and the first failing construct explicit and deterministic |
| D-033 | Relocations are applied to the flattened image payload, after per-kind section buffers are merged at their final offsets | the per-kind buffers are scratch; writing a final-layout offset into them corrupts unrelated memory, and BSS is zero-filled by the image buffer rather than copied |
| D-034 | Every non-BSS data object is aligned to at least eight bytes | keeps the pinned MZ64 contract that a relocated pointer's represented value is eight-byte aligned, so the loader needs one aligned store per fixup |
| D-035 | One declarator per AST node, chained through `next`, spliced whole into statement lists | a single node per declaration statement silently dropped later declarators, which then lost their frame slot and were emitted as external references |
| D-036 | Output files are created once and written once from a fully built in-memory image; there is no temporary-file rename step | the pinned target dispatches no `rename` (56h) or `delete` (41h) service, so the temporary protocol could never run on target; a truncated write stays detectable through the CC64O and MZ64 checksums |
| D-037 | `cc64_create` and `cc64_lseek` join the emitted service thunks, using the target's RAX result and CF error convention | the compiler must open and seek real files, and both services are already listed in the pinned ABI table; the file-descriptor result register is read back from the target source rather than assumed from classic DOS |
| D-038 | A variadic function spills the six integer argument registers into a save area in its own frame, and `va_arg` steps eight bytes per slot | an eight-byte slot stride is required to read whole registers and to keep the walk independent of the argument type; a `sizeof`-based stride silently misreads every `int` variadic argument |
| D-039 | Only the end of a whole preprocessing run emits an end-of-file token | the lexer ends every source, so copying an included file's end token into the output truncated `-E` and `--dump-tokens` at the first include |
| D-040 | Array bounds, enumerators, and case labels are evaluated by a project integer constant-expression evaluator | accepting only a bare literal rejected ordinary spellings such as `[4 * 2 + 1]`, a macro, or `sizeof`, including in CC64's own sources |
| D-041 | Floating constants are converted by project code with a 128-bit intermediate and round-to-nearest-even | a bootstrap build and a self-hosted build must emit identical objects, so the conversion cannot depend on whichever library each build links; values outside the documented range are diagnosed rather than misrounded |
| D-042 | An array or function member decays to its address, like a bare identifier of that type | loading a member of array type produced the first element's value instead of the member address |
| D-043 | The target startup routine is emitted into every image that defines `main`, and the link trampoline calls it | the old trampoline passed a pointer to the process prefix where `argv[0]` belonged, so no target program could read its own name or arguments; a required startup symbol makes the entry path part of the object contract |
| D-044 | The startup copies the command tail into its own frame, terminates it, and splits it in place on spaces and tabs; the length field is read as a byte | the prefix stores a one-byte length, so reading a quadword would copy unrelated prefix bytes; scanning to the terminating NUL removes any bound on the tokenizer loop |
| D-045 | Every hand-encoded startup instruction is checked against the assembler byte for byte | four separate encodings in the first draft were wrong or invalid (`C7 /1`, a reversed 8-bit move, a spurious REX bit), and an invalid opcode silently hung the target |
| D-046 | Section alignment padding is applied to the output buffer before the section payload is appended | the previous order appended the padding after the offset had already been aligned, doubling every gap and leaving sections at unaligned offsets that broke eight-byte data relocations |
| D-047 | The startup copies the command tail with an explicit byte loop instead of a repeated-string move | the startup runs before any library code can establish the direction flag, so a repeated move would depend on state the image itself has not set; the loop also gives the copy a bound the instruction does not |
| D-048 | A relational operator selects the condition code for the operand order the comparison actually computes, and signedness selects between the signed and unsigned condition | the table mixed operand orders, so unsigned `>` behaved as `>=` and unsigned `>=` behaved as `!=`; a loop guarded by an unsigned `>` then never terminated, which is why the target formatter hung while the host build passed |

## Review rule

Every new non-obvious decision receives a row before implementation. A design
that requires studying unrelated source is rejected and redesigned from its
specification. `tests/audit.py` checks tracked source, forbidden artifact
patterns, provenance records, and clean milestone documentation. Release
review also records tool versions and performs a clean deterministic rebuild.

