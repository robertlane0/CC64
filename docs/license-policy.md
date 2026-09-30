# CC64 license policy and inventory

CC64 source and tests are original project contributions distributed under the
repository MIT license. The target checkout is not bundled, copied, or
subordinated as a CC64 dependency.

Permitted inputs are language and ABI specifications, processor manuals,
project-authored interface contracts, and project-authored tests. External
source archives, compiler-generated target artifacts, and target-runtime
source are not release inputs. Every imported file must have a recorded origin
and compatible license before it can be considered.

A generic host C compiler is a bootstrap tool only. Its source, headers,
libraries, and generated artifacts do not enter CC64 or target images.

## Inventory

Every file the repository tracks is listed below with the license it carries.
The list is exhaustive: `tests/audit.py` fails the release if a tracked file
is not covered by one of the rows, so a new file cannot be added without a
license decision.

| Path | License | Origin |
|---|---|---|
| `LICENSE` | MIT | this project's own terms |
| `Makefile` | MIT | this project |
| `.gitignore` | MIT | this project |
| `AGENTS.md` | MIT | this project's own plan |
| `README.md` | MIT | this project |
| `docs/**` | MIT | this project |
| `include/cc64/**` | MIT | this project |
| `include/target/**` | MIT | this project |
| `src/backend/**` | MIT | this project |
| `src/common/**` | MIT | this project |
| `src/driver/**` | MIT | this project |
| `src/frontend/**` | MIT | this project |
| `src/ir/**` | MIT | this project |
| `src/linker/**` | MIT | this project |
| `src/parser/**` | MIT | this project |
| `src/runtime/**` | MIT | this project |
| `src/semantic/**` | MIT | this project |
| `src/selfhost/**` | MIT | this project |
| `src/cc64.h` | MIT | this project |
| `tests/**` | MIT | this project |
| `tools/**` | MIT | this project |

## Bootstrap tool

The host build uses a generic host C compiler, GNU Make, and Python 3. They
are build tools: their source, headers, libraries, and generated artifacts do
not enter CC64 or any target image, and nothing they produce is a release
input. The QEMU and Bochs packages used by the target gates are likewise test
tools, not build or runtime dependencies of a target program.

## Target checkout

`MS-DOS64` is an interface reference and a test platform. It is ignored by
this repository, is not a build input, and contributes no file to a release.
The changes CC64 depends on are made on that repository's `edit` branch and are
recorded with their reason and observable effect in
`docs/provenance-ledger.md`.

Two more target changes are made on that branch and are recorded the same way.
D-135 at `b850bbf` doubles the kernel slot to 512 sectors, which moves the
target's volume from LBA 512 to 1024. D-137 at `11b3cbb` fixes the target's
date setter, which wrote the month while still holding a day the new month
could not have; the target's own suite goes from 94 passed and 1 failed to 95
passed and none. Neither is required to make the compiler operational on its
own, and neither is derived from CC64.

The one CC64 depends on for self-hosting is the heap extension, recorded as
D-132: the target's `edit` branch at `d9a4379`, over the pinned reference
`13c3ced`, provides a twelve-mebibyte heap where the reference provides six.
The kernel slot was then doubled to 512 sectors as D-135, at `b850bbf`, which
moved the target's volume from LBA 512 to 1024. `make target-heap` measures the
heap difference with a compiler-produced program and requires at least eleven
mebibytes, so the pinned reference fails that gate by name rather than the
dependency being implicit. The pinned reference revision stays in the checkout,
so a CC64 build can always be compared against the unmodified target.

The volume's address is read from the image rather than written into CC64's
harnesses (D-134), so a target that moves it is followed instead of
contradicted. `tools/target_revision.py` owns that read.

c-edit, the large program CC64 compiles, is an external repository and not a
CC64 build input. Its `ms-dos64` branch carries one change of its own, made
with permission and confined to the memory layer: an arena or gap buffer whose
reservation the platform cannot honour falls back to a fixed small one (36
lines in three files, no behaviour change on a host). That change is c-edit's
and is not copied into CC64; CC64 records the observation in
`docs/provenance-ledger.md` and `docs/status.md`.
