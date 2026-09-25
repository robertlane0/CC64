# CC64 license policy

CC64 source and tests are original project contributions distributed under the
repository MIT license. The target checkout is not bundled, copied, or
subordinated as a CC64 dependency.

Permitted inputs are language and ABI specifications, processor manuals,
project-authored interface contracts, and project-authored tests. External
source archives, compiler-generated target artifacts, and target-runtime
source are not release inputs. Every imported file must have a recorded origin
and compatible license before it can be considered; the current implementation
has no such imports.

A generic host C compiler is a bootstrap tool only. Its source, headers,
libraries, and generated artifacts do not enter CC64 or target images.
