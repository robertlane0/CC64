# Clean-room review checklist

- [ ] Reviewer confirms the change was derived from a written CC64 contract or
      a language/processor specification.
- [ ] No external compiler, OS, tutorial, or unrelated repository source was
      copied, translated, decompiled, or mechanically rewritten.
- [ ] New non-obvious choices have a provenance-ledger entry.
- [ ] Tests are project-authored and do not depend on another compiler's
      undocumented output.
- [ ] Diagnostics, limits, and failure cleanup are covered.
- [ ] Generated objects, images, and temporary files are absent from the
      tracked change.
- [ ] `python3 tests/audit.py`, `make check`, and the relevant target matrix
      pass before merge.
