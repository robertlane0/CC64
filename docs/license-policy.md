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
The two changes CC64 depends on are made on that repository's `edit` branch and
are recorded with their reason and observable effect in
`docs/provenance-ledger.md`.
