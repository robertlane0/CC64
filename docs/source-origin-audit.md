# Source-origin audit procedure

Run before every release and whenever compiler code changes substantially.

1. Build from a clean checkout with the documented host bootstrap command.
2. Run unit, semantic, object, negative, deterministic, and target tests.
3. Inspect the tracked-file list for external source archives, generated
   objects, executable images, package caches, and vendored compiler trees.
4. Search tracked source and tests for compiler-project names, copied notice
   blocks, machine-code byte blobs, and external repository URLs.
5. Map every non-obvious implementation choice to a specification or a row in
   `docs/provenance-ledger.md`.
6. Review new tests to ensure expected behavior follows C17 or a pinned
   project contract rather than a host compiler's undocumented output.
7. Record the audit date, commit, bootstrap tool version, commands, and result
   in `docs/status.md`.
8. Reject the release if any implementation dependency has unknown origin or if
   a normal target build invokes a host assembler, linker, or code generator.

The automated audit is necessary but does not replace the human design review.
