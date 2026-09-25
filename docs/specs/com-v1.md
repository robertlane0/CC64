# Raw `.COM` image contract, version 1

Status: normative for target `x86_64-pc-dos64`.

A version 1 raw `.COM` file is one flat payload. Its first byte is loaded at
`PSP + 0x2a0`; that address is the entry and has 16-byte alignment. The file
must not begin with the `MZ64` magic. There is no separate BSS description,
load-time relocation table, or non-entry payload convention.

The payload contains text, read-only bytes, and initialized writable data in
link order. Each section is aligned as required, and the linker inserts zero
padding. Automatic and uninitialized file-scope storage use stack or explicit
runtime allocation; there is no implicit host-runtime initialization.

A raw image is limited to 16 MiB, must fit in target process memory, and must
resolve all PC-relative and intra-image absolute references at link time. An
absolute data pointer that would change under load bias is rejected for raw
output. A caller can request `.MZ64` when such pointers are required.

The image has no checksums or mandatory trailer in version 1. Deterministic
output consists of exactly the linked payload bytes. A failed compile or link
must remove or never create the requested output.
