# CC64 validation matrix

Recorded on 2026-09-24 with the host bootstrap tool and the available target
checkout.

| Gate | Result |
|---|---|
| clean host build | passed |
| unit, frontend, semantic, driver, object, raw/MZ64 integration | passed |
| deterministic raw and MZ64 links | passed |
| compiler-produced QEMU raw cases | passed |
| compiler-produced QEMU MZ64 data-pointer case | passed |
| compiler-produced QEMU stage1 smoke | passed (`Exit 51`) |
| Bochs | not run; `bochs` is absent from the configured Pacman repositories |
| target volume cleanliness | covered by the target checkout's existing checks; repeated here through fresh lean images |

`make check-release` is the aggregate local gate. A release must record the
Bochs result rather than silently treating an unavailable emulator as a pass.
