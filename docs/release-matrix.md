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
| target-profile self-host probe | passed (15 production translation units compile individually) |
| deterministic malformed source/object/image smoke | passed (46 source cases, 20 bit mutations, 4 CRC-valid structural mutations, truncation and image fixups) |
| clean-build reproducibility | passed; two path-independent clean exports and target artifacts match |
| automated provenance/source-origin audit | passed; human review/signoff remains open |
| full QEMU/Bochs target matrix | partial; QEMU has 7 cases and Bochs has 1 raw case; MZ64/alternate-emulator coverage remains open |
| Bochs | passed (`Exit 7`, raw `.COM`, Bochs 3.1) |
| target volume cleanliness | passed before/after each QEMU case using the target checkout's `check_volume_clean.py`; stage1 and Bochs paths use the same check |

`make check-release` is the aggregate local gate. `make check-release-strict`
turns missing emulator evidence and a dirty source tree into failures. Both
gates now have QEMU and Bochs evidence, but neither is a release approval
while M7 full self-hosting and the complete conformance matrix remain open.
