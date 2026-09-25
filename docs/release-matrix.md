# CC64 validation matrix

Recorded on 2026-09-24 with the host bootstrap tool and MS-DOS64 revision
`13c3cedb05ad75592c17bf2006ba8617c8761a38`. The target scripts reject a
mismatched checkout rather than silently testing a different interface.

| Gate | Result |
|---|---|
| clean host build | passed |
| unit, frontend, semantic, driver, object, raw/MZ64 integration | passed |
| deterministic raw and MZ64 links | passed |
| compiler-produced QEMU raw cases | passed (7 cases, including exit/open services) |
| compiler-produced QEMU MZ64 data-pointer case | passed (`Exit 7`) |
| compiler-produced QEMU stage1 smoke | passed (`Exit 51`; bootstrap smoke, not full self-hosting) |
| deterministic malformed source/object/image smoke | passed (46 source cases, 20 bit mutations, 4 CRC-valid structural mutations, truncation and image fixups) |
| clean-build reproducibility | passed; two path-independent clean exports and target artifacts match |
| automated provenance/source-origin audit | passed; human review/signoff remains open |
| full QEMU/Bochs target matrix | blocked; QEMU coverage is recorded, Bochs is unavailable |
| Bochs | skipped; `bochs` is absent from the configured Pacman repositories |
| target volume cleanliness | passed before/after each QEMU case using the target checkout's `check_volume_clean.py`; stage1 and Bochs paths use the same check |

`make check-release` is the aggregate local gate. It returns success when
Bochs is unavailable, but that result is recorded as skipped and is not a
Bochs pass. `make check-release-strict` turns missing emulator evidence and a
dirty source tree into failures. Neither gate is a release approval while M7
full self-hosting and the required alternate-emulator evidence remain open.
