# CC64 target ABI, version 1

Status: normative for target `x86_64-pc-dos64`, ABI `cc64-dos64-v1`.

## Machine and memory

CC64 emits x86-64 long-mode machine code for flat little-endian linear memory.
Code and data use signed 32-bit RIP-relative displacement. A target image must
fit below `0x80000000`; the initial compiler limit is 16 MiB.

There is no red zone. Interrupt and exception paths share the process stack.
At a call instruction, `RSP` is 16-byte aligned. Consequently, on function
entry `RSP % 16 == 8`; the return address is at `[RSP]` and the first stack
argument is at `[RSP+8]`. A leaf must restore `RSP` before returning.

## Calling convention

The integer and pointer convention is System V AMD64:

| Position | Register | Position | Register |
|---:|---|---:|---|
| 1 | `RDI` | 4 | `RCX` |
| 2 | `RSI` | 5 | `R8` |
| 3 | `RDX` | 6 | `R9` |

Arguments after six are pushed right-to-left in 8-byte slots. The caller adds
padding so `RSP` is 16-byte aligned before `CALL`. Integers return in `RAX`,
pointers in `RAX`, and `void` has no return value. `RDI` and `RSI` follow the
usual callee-saved rule.

`RBX`, `RBP`, and `R12` through `R15` are preserved by every callee.
`RSP` is preserved modulo argument removal. `RAX`, `RCX`, `RDX`, `R8` through
`R11`, and all flags are call-clobbered. `XMM0` through `XMM7` are used for
`float`; `XMM0` through `XMM7` and `XMM0` return `double`. This first ABI
does not yet define aggregate parameter passing by value; aggregates are
deferred until their representation is pinned.

## Variadic calls

A call to a function declared with `...` sets `AL` to the number of vector
registers that carry arguments, after the integer argument registers are
loaded and before the `CALL`. A variadic function spills the six integer
argument registers into a 48-byte register save area in its own frame.

`va_start` returns the first slot after the function's named integer
parameters. `va_arg` reads one whole eight-byte slot and advances eight bytes,
narrowing the value to the requested type; the slot stride is fixed by this
contract and is not `sizeof` of the argument type. Floating variadic
arguments are not part of this ABI revision and are diagnosed by the target
library rather than silently misread.

## Basic types

| C spelling | Size | Alignment | Representation |
|---|---:|---:|---|
| `_Bool`, `char` | 1 | 1 | byte; plain `char` is signed |
| `signed char` | 1 | 1 | two's complement |
| `unsigned char` | 1 | 1 | modulo 256 |
| `short` | 2 | 2 | two's complement |
| `unsigned short` | 2 | 2 | modulo 65536 |
| `int` | 4 | 4 | two's complement |
| `unsigned int` | 4 | 4 | modulo 2^32 |
| `long`, pointer | 8 | 8 | two's complement / linear address |
| `unsigned long` | 8 | 8 | modulo 2^64 |
| `long long` | 8 | 8 | two's complement |
| `unsigned long long` | 8 | 8 | modulo 2^64 |
| `float` | 4 | 4 | IEC 60559 binary32, round-to-nearest |
| `double` | 8 | 8 | IEC 60559 binary64, round-to-nearest |

An object pointer is a 64-bit linear address. Function pointers have the same
size and representation. `NULL` is integer zero after conversion. Conversion
from an integer to a pointer and dereference obey the selected C17 subset;
unsupported provenance behavior is diagnosed rather than extended.

## Program entry and startup

The loader copies the payload to a 16-byte-aligned process image. For a raw
`.COM`, payload offset zero is the entry. For `MZ64`, `entry_offset` selects
it. At entry:

- `RDI` is the linear address of the 672-byte process prefix (PSP).
- `RSP` is 16-byte aligned and points to loader-owned termination state.
- callee-saved state is unspecified; the startup code establishes it.
- no C runtime is implicitly linked.

The freestanding startup routine obtains the command tail from the target PSP:
the byte at `PSP+0xa0` is the tail length and bytes at `PSP+0xa1` are the tail,
with no terminating NUL guaranteed by the loader. Version 1 emits a bounded
compatibility view: it reports one argument when the tail is empty and exposes
the tail as the second argument when present; a later ABI revision will
tokenize all arguments and pass validated environment pairs. The application
receives at least 64 KiB of loader-owned stack below its entry stack top; the
startup routine aligns that stack before calling `main`. It passes the `int`
return to the target exit boundary. A bare `RET` from a raw image returns to
the loader trampoline.

## Target services

Generated code never uses Linux, Win32, BIOS, or host system calls. It may
call project runtime functions which use the target `INT 21h` contract:

| Service | Interface |
|---|---|
| console byte | `AH=02h`, `DL=byte` |
| console string | `AH=09h`, `RDX=NUL-terminated bytes` |
| allocate | `AH=48h`, `RBX=16-byte paragraphs`; `RAX=linear block` |
| free | `AH=49h`, `RDI=linear block` |
| open/create | `AH=3Dh/3Ch`, `RDX=8.3 path`, mode in `AL` |
| read/write | `AH=3Fh/40h`, `RBX=handle`, `RDX=buffer`, `RCX=count` |
| seek | `AH=42h`, `RBX=handle`, `RDX:RCX=offset`, `AL=origin` |
| close | `AH=3Eh`, `RBX=handle` |
| exit | `AH=4Ch`, low return value in `AL` |

The target has a 16 MiB conventional process area. Allocation failure is
represented by a null result. A raw `.COM` has no metadata for separate BSS;
the compiler emits statically initialized objects in-band and automatic
storage in the application frame. `MZ64` supports zero-filled BSS.

## Versioning

The target triple is `x86_64-pc-dos64`. Drivers reject any other triple and
any requested ABI other than `cc64-dos64-v1` with a driver diagnostic. Later
ABI versions must preserve version 1 images or identify their incompatibility
before opening an output file.
