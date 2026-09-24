# AGENTS.md: Plan for an Original C Compiler for MS-DOS64

## Mission

Create **CC64**, an independently designed C compiler and native toolchain for
the 64-bit MS-DOS64 environment. The first release is a host-built compiler
that produces freestanding x86-64 programs for the target operating system.
The long-term goal is a self-hosting compiler that can also be built and run
on MS-DOS64.

The compiler must be an original implementation. It must not copy, translate,
adapt, mechanically transform, decompile, or embed source code, generated
tables, machine-code fragments, or implementation ideas taken from another C
compiler or from an unrelated project.

## 1. Non-negotiable originality policy

These rules apply to compiler code, headers, tables, linker code, runtime code,
tests, documentation examples, and build scripts:

- Do not copy or adapt GCC, Clang, MSVC, TinyCC, chibicc, 8cc, lacc, PCC,
  or any other compiler implementation.
- Do not copy code from operating systems, libraries, tutorials, sample
  projects, disassembly, decompiler output, or unrelated repositories.
- Do not vendor compiler source, import a compiler as a dependency, use
  compiler-generated object files as source material, or check generated
  machine code into the repository.
- Do not use another compiler to produce the normal target build or to bless
  CC64 output. A generic host C compiler may be used only as an explicitly
  documented bootstrap tool for compiling the original CC64 source; its source,
  headers, libraries, and generated artifacts must not become part of CC64 or
  its target programs.
- Existing MS-DOS64 source may be consulted only to identify externally
  observable interfaces and test behavior. CC64 must not import, translate, or
  mechanically rewrite that source. Compatibility must be implemented from a
  written contract and original test cases.
- Permitted inputs are language and ABI specifications, processor manuals,
  project-owned interface documentation, and tests written specifically for
  this project. Record the origin of every non-obvious design decision.
- If a proposed implementation resembles existing code, stop and redesign it
  from the specification. Add a provenance note explaining the independent
  derivation.

Maintain a provenance ledger and run a source-origin audit before every
release. No external source archive or compiler-generated artifact may be
required to build CC64.

## 2. Target contract

The first compatibility target is the existing MS-DOS64 x86-64 long-mode
environment. Pin the following contract in a versioned specification before
writing the backend:

- Flat 64-bit addressing, little-endian x86-64, and the System V AMD64 integer
  calling convention.
- Integer arguments in `RDI`, `RSI`, `RDX`, `RCX`, `R8`, and `R9`; additional
  arguments on the stack; return value in `RAX`; `RBX`, `RBP`, and `R12`–`R15`
  preserved by callees.
- A 16-byte stack alignment contract, near calls only, and no red zone. The
  target shares its stack with interrupt and exception paths, so the compiler
  must not assume a red zone exists.
- A documented startup contract for `argc`, `argv`, environment, stack size,
  entry point, and the DOS64 exit path. The runtime must translate these values
  without depending on a host C runtime.
- A documented executable contract for raw `.COM` images and, after the first
  milestone, the target's `MZ64` image form. The contract must cover entry
  offset, file image size, memory size, section/data initialization, stack
  requirements, load bias, relocations, and failure handling.
- A documented system-call boundary for memory allocation, console I/O, file
  handles, seeking, process exit, and diagnostics. Generated programs must use
  that boundary and must not contain host-OS system calls.

The target specification may evolve. Version it, keep old images readable
where practical, and make the compiler reject unsupported target versions with
a clear diagnostic.

## 3. Scope and language policy

### Initial language target

Implement a deliberately small, testable subset of C17 first:

- Preprocessor translation phases, comments, line splicing, identifiers,
  character/string literals, integer and floating constants, and source
  locations.
- Object and function declarations, typedefs, storage classes, scopes,
  linkage, function definitions, parameters, and return types.
- Expressions with the usual arithmetic, integer, pointer, assignment,
  conditional, comma, and sequence semantics.
- `if`, `else`, `while`, `do`, `for`, `switch`, `break`, `continue`, `return`,
  and `goto` with a documented restriction on how labels may be used.
- `void`, `char`, signed and unsigned integer types, `float` and `double`
  after the integer backend is stable, pointers, arrays, functions, enums,
  structs, unions, bit-fields only after their layout rules are specified,
  `const`, `volatile`, `restrict`, and `static_assert` where applicable.
- Static and automatic storage, zero initialization, string and aggregate
  initialization, and externally visible symbols.
- A small freestanding library for memory, strings, formatted output, file
  I/O, and process termination. The library is part of CC64 and is written
  against the target contract.

### Explicitly deferred

Do not attempt the whole C language in the first release. Defer variable
length arrays, `_Atomic`, threads, threads-local storage, complex and
imaginary types, variable-size aggregates, full `long double`, arbitrary
inline assembly, dynamic libraries, exception handling, and a hosted POSIX
environment. Each deferred feature requires a written design and tests before
implementation begins.

Start with `-O0` and correctness-first code generation. Add optimization only
after the semantic and ABI test suites pass.

## 4. Proposed repository structure

Create the implementation under the tracked `CC64` repository - do not modify the MS-DOS64 OS target until the interfaces are
stable:

```text
src/
  driver/       command line, source manager, diagnostics
  frontend/     lexer, preprocessor, parser, AST
  semantic/     scopes, types, constants, checking
  ir/           original typed intermediate representation
  backend/      instruction selection, register allocation, encoding
  linker/       CC64 object reader/writer, layout, relocations
  runtime/      freestanding target startup and library
include/cc64/   target and compiler-interface headers
tests/          unit, semantic, object-format, and integration tests
docs/           specifications, provenance, and design records
```

Keep generated objects, images, temporary files, and compiler output outside
version control. Build scripts must work from a clean checkout and must not
silently download source or tools.

## 5. Compiler architecture

Implement the compiler as a pipeline with explicit ownership and diagnostics:

```text
source bytes
  -> source manager and translation-unit state
  -> lexer
  -> preprocessor
  -> parser and AST
  -> semantic analysis and type checking
  -> typed IR
  -> instruction selection and register allocation
  -> instruction encoder and CC64 object writer
  -> linker and image emitter
  -> DOS64 loader/runtime
```

### Driver and diagnostics

The driver owns option parsing, phase ordering, include paths, output files,
and exit status. Diagnostics must include source file, line, column, phase,
and a stable error identifier. A failed phase must not leave a partial target
image that could be mistaken for a successful build.

Use a small project-owned diagnostic formatter rather than an external
compiler framework. Preserve source spans through every phase so later errors
point to the original C token.

### Lexer and preprocessor

The lexer must preserve byte-level information needed for C string and
character escapes, line splicing, and phase conversion. It must distinguish
keywords from identifiers only after preprocessing rules have been applied.

The preprocessor owns macro expansion, function-like macro argument capture,
conditional inclusion, `defined`, `#include`, `#line`, `#error`, and the
standard predefined macros required by the selected dialect. Define recursion,
blue-paint, expansion-depth, and include-cycle behavior explicitly and test
the limits. Do not evaluate C expressions in the preprocessor until constant
expression semantics are available.

### Parser and semantic model

Build a syntax tree that retains source spans and declaration context. The
parser must reject malformed input without crashing, produce useful errors,
and recover sufficiently to report multiple independent errors.

The semantic layer owns:

- A type representation for integers, floating types, pointers, arrays,
  functions, structs, unions, enums, qualifiers, and incomplete types.
- Declaration and definition rules, storage duration, linkage, scope, and
  name lookup.
- Array-to-pointer and function-to-pointer conversion, integer promotions,
  usual arithmetic conversions, pointer compatibility, and assignment rules.
- Constant expressions, enum values, case labels, bit-field layout, and
  alignment requirements.
- Evaluation of expression side effects and sequence points before lowering.
- A precise policy for undefined behavior and implementation-defined behavior;
  every diagnostic must identify the policy rather than silently inheriting a
  different compiler's behavior.

Do not encode host compiler behavior as a shortcut. Where C leaves a choice
to the implementation, document the CC64 choice and add a conformance test.

### Intermediate representation

Define a typed IR before optimizing or generating assembly. The first IR may
be close to structured control flow, but it must have explicit integer widths,
signedness, pointer widths, memory effects, and source locations.

Lower declarations, expressions, and statements into IR while preserving C
semantics. Give every function a clear local-frame model, represent calls and
external symbols explicitly, and make undefined behavior and undefined symbols
visible to later checks. Do not make the backend depend on parser data
structures.

### Backend

The initial x86-64 backend should prioritize correctness and inspectable code:

- A target description containing registers, instruction forms, memory
  operands, stack layout, and relocation capabilities.
- A deterministic instruction encoder independent of an external assembler.
- Correct handling of signed and unsigned extension, narrow loads/stores,
  integer arithmetic, comparisons, branches, calls, returns, stack arguments,
  and aggregate layout.
- Local variables and temporaries assigned stack slots first; a simple
  register allocator can be added after the frame model is tested.
- Clear handling of `volatile` accesses and externally visible calls.
- A machine-code inspection mode for tests and debugging, with no hidden
  dependency on another compiler.

Use PIC/PC-relative references where the target image permits them. For
load-biased images, emit explicit relocation records rather than silently
resolving addresses to the link-time base. Every relocation must be bounded,
typed, and validated by the linker.

### Object format and linker

Create a versioned project-owned object format, tentatively called `CC64O`.
It must contain a magic/version, target triple, section table, symbol table,
alignment and section flags, relocations, and an optional source/debug map.
Define signedness, width, overflow checks, and end-of-file validation before
implementing the writer.

The linker must:

1. Read and validate every object.
2. Merge sections and resolve symbols with explicit conflict diagnostics.
3. Apply PC-relative, absolute, section-relative, and data-pointer
   relocations with overflow checks.
4. Lay out text, read-only data, initialized data, and zero-filled BSS.
5. Emit a deterministic raw `.COM` image first.
6. Emit the documented `MZ64` form, including memory size and relocation
   metadata, once the loader contract is tested.

The linker must reject unresolved symbols, overlapping sections, malformed
records, unsupported relocations, and images that exceed the target budget.
It must not invoke a host linker, `objcopy`, or another compiler as part of
the normal target build.

## 6. Runtime and target library

Write a small target runtime and freestanding headers from the documented
MS-DOS64 interfaces. The runtime is not copied from a hosted C library.

The startup path must:

1. Receive or reconstruct the target entry state.
2. Establish the required stack alignment and register state.
3. Validate and zero the BSS region.
4. Construct `argc`, `argv`, and `envp` according to the pinned contract.
5. Run target initialization only where explicitly required.
6. Call `main` and translate its return value into the target exit path.

Implement only the library functions needed by the initial language subset.
Memory allocation must use the target allocator, file operations must use the
target handle interface, and console diagnostics must use the target output
path. Do not leak host `libc` behavior or Linux syscall assumptions into target
objects.

Keep headers target-specific and self-contained. A target header must not
include a host system header by accident. Document every type width, pointer
representation, alignment, and calling convention.

## 7. Milestones and exit criteria

### M0 — Contract, provenance, and skeleton

Deliver:

- A versioned target ABI and executable specification.
- The `CC64O` object-format specification.
- A provenance ledger and source-origin audit procedure.
- A driver skeleton, source manager, diagnostic types, and a reproducible
  build/test harness.
- Original empty or minimal test fixtures with no copied compiler code.

Exit criteria: a clean build produces deterministic diagnostics and the
repository contains no unapproved external source or generated target code.

### M1 — Lexer and preprocessor

Implement translation phases, tokens, literals, macro expansion, conditionals,
includes, and predefined macros. Add unit tests for boundary cases, malformed
input, nesting, recursion limits, and source locations.

Exit criteria: the preprocessor produces deterministic token streams for the
project-authored corpus and rejects invalid input with stable diagnostics.

### M2 — Parser, types, and semantic checks

Implement declarations, expressions, statements, functions, aggregates,
scopes, linkage, constants, conversions, and the deferred-feature diagnostics.
Build an AST and type API that does not depend on a host compiler or parser
generator.

Exit criteria: valid programs in the defined C17 subset pass semantic tests;
invalid programs fail with precise diagnostics rather than crashes or silently
accepted extensions.

### M3 — IR and x86-64 code generation

Define the IR, lower the AST, implement stack-frame construction, integer
operations, control flow, calls, globals, and basic aggregate operations. Emit
`CC64O` directly and build a machine-code inspection tool for tests.

Exit criteria: freestanding functions and complete programs link to valid
objects, and all ABI/register/stack invariants are covered by unit tests.

### M4 — Linker, loader image, and runtime

Implement `CC64O` reading, symbol resolution, relocations, section layout,
raw `.COM` output, startup code, BSS initialization, and the first target
library calls.

Exit criteria: a compiler-produced raw image boots in QEMU and Bochs, returns
control correctly, and produces deterministic output without host-OS calls.

### M5 — Language coverage and MS-DOS64 compatibility

Add structures, unions, enums, pointers, arrays, static/global data,
initializers, switch lowering, floating-point support, and the remaining
selected C17 features. Add file and process tests against the target contract.

Exit criteria: the project-authored conformance corpus runs on both target
emulators, with documented exit codes and no unresolved relocations.

### M6 — `MZ64`, diagnostics, and hardening

Implement the extended executable format and its relocation records. Improve
diagnostic recovery, resource limits, overflow checking, fuzzing, and
reproducible output. Test load at multiple biases and malformed images.

Exit criteria: both executable forms pass positive and negative tests, and
the compiler rejects all unsupported or unsafe inputs deterministically.

### M7 — Self-hosting

Reduce the compiler's own implementation to the supported language subset,
compile CC64 with CC64, compare behavior and output with the bootstrap build,
and fix discrepancies without importing another compiler's code. Then build
an MS-DOS64-hosted compiler image.

Exit criteria: a clean source tree can bootstrap and self-host without using
another compiler's source, generated code, or runtime; the resulting compiler
can compile the original conformance corpus on the target.

### M8 — Release quality

Complete provenance review, license inventory, reproducible-build checks,
performance measurements, fuzzing, documentation, and the QEMU/Bochs test
matrix. Publish only artifacts whose complete source provenance is known.

## 8. Testing strategy

### Frontend and semantic tests

Author every test input and expected result for this project. Cover token
boundaries, preprocessor expansion, declarations, scopes, conversions,
constant expressions, sequence points, aggregate layout, and each diagnostic
class. Include deliberately malformed inputs and resource-exhaustion cases.

### Backend and object tests

Decode `CC64O` with an independent project-owned reader, inspect instruction
widths and relocation records, and test signedness, stack alignment, calls,
tail positions, globals, BSS, load bias, and alignment. Do not use another
compiler as an oracle or compare its assembly as a golden file.

### Integration tests

Run compiler-produced programs in QEMU and Bochs. Start with return values,
recursion, globals, pointers, strings, structs, switch, file handles, memory
allocation, command-line arguments, and exit codes. Check serial/VGA output,
fault behavior, and volume cleanliness after every test.

### Property and security tests

Generate random token streams, malformed object records, extreme constants,
deep nesting, and relocation patterns. Enforce bounded memory, recursion,
macro expansion, and file-size limits. Treat all input and image data as
untrusted.

### Quality gates

Every milestone must pass:

- Clean build from a fresh checkout.
- Unit, semantic, object, and integration tests.
- QEMU and Bochs runs for target images.
- Deterministic output check.
- No external-source, license, or generated-artifact audit failure.

## 9. Principal risks and mitigations

- **Language scope is too large:** freeze the C17 subset, deliver one vertical
  slice at a time, and defer features explicitly rather than silently weakening
  semantics.
- **Target ABI drift:** version the ABI and executable documents, keep a
  compatibility matrix, and test both old and new image contracts when the
  loader changes.
- **Relocation and slide-safety bugs:** retain relocation records through
  linking, test multiple load biases, and reject unsupported absolute data
  pointers.
- **Premature optimization:** optimize only measured hot paths after semantic
  and ABI tests are green; keep an unoptimized reference path.
- **Resource exhaustion:** impose explicit limits on include depth, macro
  expansion, nesting, symbol count, object size, and generated image size.
- **Provenance contamination:** keep an auditable source ledger, prohibit
  external compiler source and artifacts, and require review of new design
  documents.
- **Bootstrap ambiguity:** distinguish a permitted host build tool from target
  dependencies, avoid checking bootstrap outputs, and make self-hosting an
  explicit milestone.

## 10. Definition of done

The first releasable CC64 version is complete when:

- A clean checkout builds without downloading or copying source code.
- The compiler implements the documented C17 subset and rejects unsupported
  features clearly.
- All compiler, linker, runtime, and test code has recorded original
  provenance.
- The normal target build uses CC64's own object writer and linker; it does not
  invoke another C compiler, assembler, linker, or code generator.
- Compiler-produced `.COM` and `MZ64` images run under both QEMU and Bochs.
- ABI, relocation, BSS, startup, exit-code, file-I/O, and error-path tests pass.
- Output is deterministic, bounded, and free of host-OS assumptions.
- The conformance and provenance audits pass with no copied or derived
  implementation code.

Self-hosting is a release milestone for the mature compiler, not a reason to
copy code from an existing compiler into the first implementation.

## 11. First implementation sequence

1. Write the target ABI, `CC64O`, `.COM`, and `MZ64` contracts.
2. Create the provenance ledger, license policy, and clean-room review checklist.
3. Build the driver, source manager, arena allocator, and diagnostic formatter.
4. Implement and test the lexer.
5. Implement and test the preprocessor.
6. Implement the parser, type system, and AST.
7. Define the IR and write semantic lowering tests before code generation.
8. Implement the x86-64 encoder, frame layout, and object writer.
9. Implement the linker and raw `.COM` path, then boot the first target image.
10. Add the runtime library, target I/O, and compiler-produced conformance tests.
11. Add `MZ64`, floating point, optimization, fuzzing, and self-hosting one
    measured milestone at a time.
