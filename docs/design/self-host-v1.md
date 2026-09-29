# Bootstrap and self-host validation

## Stages

The first target-side gate uses `src/selfhost/stage1.c`, an original small C
program with its own deterministic parser and result checksum. The bootstrap
compiler emits its `CC64O` and `MZ64` image, and the target loader runs the
resulting stage under QEMU. `make self-host` builds and runs the stage twice,
compares the two images, and records the expected `Exit 51` result. The
adjacent target checkout is revision-pinned by the test scripts; when an
emulator is unavailable, the local gate reports a skip, while strict release
mode rejects that condition.

That gate is intentionally separate from host `make`: it proves that target
objects, `MZ64` relocation, startup, integer calls, local aggregate and string
initialization, and target execution work together without a target assembler
or linker.

`make selfhost-probe` compiles every production translation unit individually
with the target header profile, so the first construct the compiler cannot
lower is named rather than found at link time.

`make self-host-stage` is the first self-hosting gate. The bootstrap compiler
builds a complete target image of itself, that image is staged on a volume with
the project's own sources and headers, and it compiles those sources on the
target with its own front end, lowering, and encoder. Every object it produces
is read back and compared byte for byte with the bootstrap compiler's, and the
self-hosted linker then links those objects into a compiler image that is
compared with the bootstrap-built image. The compiler builds itself and the
product is the same compiler.

`make self-host-run` closes the loop on a single program: a target-built
compiler compiles a project-authored source, links it with its own linker, and
the linked program is executed, with the object, the image, and a repeated run
all compared with the bootstrap build.

`make self-host-corpus` is the language-suite half of the milestone. A
target-built compiler compiles every case of the target conformance corpus,
its own linker links each one, and every resulting image is run and compared
with the exit code and output the corpus records. Each object is also compared
byte for byte with the bootstrap compiler's, so a construct the front end
lowers differently is caught even when the program happens to produce the same
exit code.

## Why the cases run in batches

The target volume holds no directories and its data area is smaller than the
compiler image, the whole corpus, and every output at once. The preprocessor's
documented base-name fallback resolves a qualified include on a flat volume,
and the shell's command tail is a fixed 143 bytes, so a case is staged under a
one-letter name and a command names one source and one object. Each batch
stages the image, the headers, and the sources it holds, and the objects are
read back and released before the next batch is staged.

## What is still not covered

A target-built compiler reproducing the corpus byte for byte is strong
evidence that the compiler is a fixed point on every construct the corpus uses.
It is not a claim that the corpus is exhaustive: the corpus covers the
constructs the version 1 subset accepts, not every program expressible in it.
A defect that needs a construct no case uses is not excluded by this gate, and
the milestone records the corpus as the language suite rather than as a proof
of completeness.
