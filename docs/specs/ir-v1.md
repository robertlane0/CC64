# CC64 typed IR, version 1

The semantic AST is lowered into a typed, explicit IR before machine-code
selection. IR instructions carry a C type, access width, signedness, source
location, symbol references, and explicit control-flow labels. The backend
consumes only this IR and never parser tokens or AST declarations.

At `-O0`, expression results are materialized in `RAX`; nested binary
expressions use balanced machine-stack pushes. Calls use the six SysV integer
registers, and a temporary alignment adjustment is emitted when an enclosing
expression has an odd stack depth. Function parameters and locals receive
negative `RBP` slots; incoming stack parameters remain at their ABI offsets.
No red-zone slot is referenced.

The first encoder covers integer/pointer loads and stores, arithmetic, signed
and unsigned division, comparisons, casts, direct and indirect calls, calls,
returns, labels, and conditional/unconditional branches. Unsupported
constructs produce a backend diagnostic rather than an implicit host
instruction sequence.

`IR_CALL` retains the function expression and an ordered argument list.
`IR_ADDR` distinguishes local, global, and computed addresses. Relocations
are emitted as typed `CC64O` records and are not resolved by the compiler
driver.
