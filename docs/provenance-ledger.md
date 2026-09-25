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

## Review rule

Every new non-obvious decision receives a row before implementation. A design
that requires studying unrelated source is rejected and redesigned from its
specification. `tests/audit.py` checks tracked source, forbidden artifact
patterns, provenance records, and clean milestone documentation. Release
review also records tool versions and performs a clean deterministic rebuild.
