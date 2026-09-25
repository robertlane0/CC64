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

## Review rule

Every new non-obvious decision receives a row before implementation. A design
that requires studying unrelated source is rejected and redesigned from its
specification. `tests/audit.py` checks tracked source, forbidden artifact
patterns, provenance records, and clean milestone documentation. Release
review also records tool versions and performs a clean deterministic rebuild.
