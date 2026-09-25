# CC64O object format, version 1

Status: normative little-endian format emitted and consumed only by CC64.

## Design constraints

CC64O is project-owned, position-independent, and explicitly records every
value needing load-time adjustment. It is not ELF, COFF, PE, or a wrapper for
a host object format. All offsets from the beginning of the file are unsigned
32-bit. All sizes and counts are bounded before allocation or seeking.

## File header

The 64-byte header is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic bytes `43 43 36 34 4f 42 4a 00` (`CC64OBJ`) |
| 8 | 2 | format version, currently 1 |
| 10 | 2 | endian marker, currently `0x3433` |
| 12 | 2 | target ABI, currently 1 |
| 14 | 2 | machine, currently 62 (`x86-64`) |
| 16 | 4 | header size, currently 64 |
| 20 | 4 | section table offset |
| 24 | 4 | section count |
| 28 | 4 | symbol table offset |
| 32 | 4 | symbol count |
| 36 | 4 | string table offset |
| 40 | 4 | string table size |
| 44 | 4 | optional source-map offset, zero when absent |
| 48 | 4 | optional source-map size |
| 52 | 4 | flags, reserved and zero in version 1 |
| 56 | 4 | header CRC-32 over bytes 0 through 55 |
| 60 | 4 | whole-file CRC-32 over all prior bytes |

Tables and payload regions may occur in any non-overlapping order, but bytes
not described by a table must be zero. A reader rejects unknown flags,
unsupported versions, non-little-endian input, truncation, overlapping ranges,
duplicate sections, or a CRC mismatch.

## Sections

Each 40-byte section record is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | name offset in string table |
| 4 | 4 | payload offset in file, zero for BSS |
| 8 | 4 | payload size |
| 12 | 4 | relocation array offset |
| 16 | 4 | relocation count |
| 20 | 4 | required alignment power of two, at most 15 |
| 24 | 1 | kind: 0 text, 1 read-only data, 2 data, 3 BSS |
| 25 | 1 | flags, reserved and zero in version 1 |
| 26 | 2 | target section index, monotonically increasing from zero |
| 28 | 8 | reserved zero |
| 36 | 4 | section payload CRC-32, zero for BSS |

Executable and read-only sections have alignment at least 16. BSS has no
file offset and may only carry relocations targeting other sections. Duplicate
section indices, kinds, or names are errors. Object payload size is limited to
256 MiB and the total file size to 512 MiB.

## Symbols

Each 32-byte symbol record is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | name offset; zero denotes an unnamed section placeholder |
| 4 | 4 | value or section-relative offset |
| 8 | 4 | section index, or `0xffffffff` for absolute |
| 12 | 1 | binding: 0 local, 1 global |
| 13 | 1 | kind: 0 notype, 1 object, 2 function, 3 section, 4 common |
| 14 | 2 | reserved zero |
| 16 | 8 | size for objects/functions/sections, otherwise zero |
| 24 | 4 | relocation array offset for a notype place-holder |
| 28 | 4 | relocation count for a notype place-holder |

A file-scope definition is global. A static declaration or definition and all
automatic names are local. Function names have function kind; named storage
has object kind. Common storage is accepted only when no definition exists and
is allocated with its size and maximum required alignment. Zero-sized symbols
are valid.

## Relocations

Each 24-byte record is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | section-relative offset |
| 8 | 4 | symbol index |
| 12 | 4 | type |
| 16 | 8 | signed addend |
| 24 | 4 | width in bytes, currently 4 or 8 |
| 28 | 4 | reserved zero |

Version 1 relocation types are:

| Value | Meaning | Width |
|---:|---|---:|
| 1 | absolute 32-bit | 4 |
| 2 | absolute 64-bit | 8 |
| 3 | RIP-relative 32-bit displacement from relocation end | 4 |
| 4 | section-relative 32-bit | 4 |
| 5 | 64-bit image-relative data pointer | 8 |

The addend is encoded in two's complement. Readers reject an out-of-range
offset or width, unknown symbol, relocation against an undefined common
symbol, and a record crossing its section. Link output checks all signed and
unsigned 32/64-bit computations for overflow. A final 32-bit PC-relative
displacement must also fit in signed 32 bits.

## Determinism

Records are ordered by target section index; symbols are ordered local first,
then by name and value; relocations are ordered by offset. No timestamp,
absolute host path, process ID, or random padding enters version 1 objects.
Reordering source declarations that does not change symbols must produce
byte-identical objects.
