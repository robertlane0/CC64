# Clean-room review record

The checklist below is the procedure named in `AGENTS.md` section 1. Each item
records the evidence a reviewer used, so a later reader can repeat the check
rather than take the result on trust. The review covers every tracked file in
the repository.

## 1. Derived from a written contract or a specification

Every subsystem names the document that fixes its behavior, and the documents
are tracked in `docs/specs/`: the target ABI, the `CC64O` object format, the
raw `.COM` and `MZ64` image forms, the typed IR, the target runtime, and the
accepted C17 subset. A construct outside the subset document receives a
diagnostic, and the deferred-construct table in `c-subset-v1.md` names the
identifier for each one.

Reviewer check: for every file under `src/`, the header comment or the
provenance ledger row that governs the file names the contract it implements.
No file implements behavior that no document states.

## 2. No external source was copied, translated, or rewritten

The permitted inputs are the C17 standard, the Intel and AMD64 architecture
manuals, the System V AMD64 ABI specification, the target's own interface
documentation, and this project's tests. `tests/audit.py` fails the build if a
tracked file names another compiler project, links an external compiler
repository, or carries a generated or binary suffix, and the release gate runs
it three times.

Reviewer check: the tracked file list contains only `.c`, `.h`, `.py`, `.s`,
`.md`, extensionless files, and the `Makefile`. There is no vendored tree, no
archive, no machine-code blob, and no symlink. Every expected-result table in
the tests is a value this project derived from a contract, not a value observed
from another compiler.

## 3. Non-obvious choices have a provenance row

`docs/provenance-ledger.md` carries one row per decision, each with the reason
the choice was made independently. A decision that would be obvious from the
specification does not get a row; a decision that could have gone another way,
and a defect the choice fixed, does.

Reviewer check: every ledger row names an observed defect or an ambiguity, and
no row cites external source as its origin.

## 4. Tests are project-authored and independent

The unit, integration, object, and target tests state the expected result from
C17 or from a pinned project contract. The object and image checks are decoded
by project-owned readers, `tools/inspect_object.py` and
`tools/inspect_image.py`, rather than compared against another tool's view of
the same bytes. The reproducibility gate compares two independent clean builds
of this project with each other.

Reviewer check: no test compares CC64 output against another compiler's
assembly, object file, or diagnostics.

## 5. Diagnostics, limits, and failure cleanup are covered

Every deferred construct has a stable identifier and a case. The preprocessor
bounds macro expansion, include depth, and token count, and each bound has a
case in the frontend group. A phase that fails publishes no output, and the
integration group checks that a failed preprocessing leaves no file behind.
The fuzz gate feeds malformed sources, objects, and images and requires that
each is rejected without a crash and without a partial output.

Reviewer check: `tests/unit/test_frontend.c` covers the resource bounds,
`tests/unit/test_semantic.c` covers the diagnostic identifiers, and
`tests/fuzz_smoke.py` covers the malformed input classes.

## 6. No generated artifact is tracked

`.gitignore` covers the build directory, the compiler binary, object and
archive suffixes, image suffixes, logs, and caches. The audit fails on any
tracked file with a generated or binary suffix, on a symlink, and on a path
containing a build, cache, or nested-repository component. The MS-DOS64
checkout is a symlink to the adjacent interface reference and is ignored.

Reviewer check: the tracked file list is source and documentation only.

## 7. The gates pass

`python3 tests/audit.py`, `make check`, and the full target matrix are run
before each release, and the results are recorded in `docs/status.md` and
`docs/release-matrix.md` with the tool versions and the target revision used.

Reviewer check: the recorded revision and tool versions match the ones the run
that produced the record used.

## Standing limitation

The review above is an implementation review of the tracked sources against
this project's own written contracts. It is not a legal provenance opinion:
confirming that no third-party material ever entered the repository is a
question for the project's own release process, and the ledger records the
inputs this project considers permitted so that a reviewer can check them.
