# Frame and call lowering record

The first backend uses a deliberately boring frame model. A function begins
with `push rbp; mov rbp,rsp` and reserves a 16-byte-aligned local area. The
first six integer parameters are copied from `RDI`, `RSI`, `RDX`, `RCX`, `R8`,
and `R9` into negative frame slots. Additional parameters are read at their
ABI stack offsets. Every exit restores `RSP` from `RBP`, pops `RBP`, and
returns.

Expression evaluation uses `RAX` and caller-saved scratch registers. A binary
node pushes its left result, evaluates its right result, moves it to `RCX`,
then pops the left result. Balanced pushes make the depth explicit to call
emission, which inserts an eight-byte adjustment when required.

The model is observable in object inspection: prologue stores, frame sizes,
call register order, and relocation fields can be checked without a host
assembler. It is not yet an optimization strategy; a later register allocator
must preserve these externally visible ABI tests.
