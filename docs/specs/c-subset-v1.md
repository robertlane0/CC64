# CC64 C17 subset, version 1

This document defines the language surface accepted by the first compiler
pipeline. It is a subset of C17, not an extension.

## Accepted declarations

- `void`, `_Bool`, character, short, integer, long, long-long, float, and
  double types with the ABI widths in `abi-v1.md`.
- Pointers, arrays with constant bounds, function types, structs, unions,
  enums, typedefs, `const`, `volatile`, and `restrict`.
- File-scope declarations and definitions; `static` and `extern` linkage;
  block-scope automatic and register objects; typedefs in block scope.
- Parameters, including `void` parameter lists and ordinary named parameters.
- Scalar and aggregate initializer lists. Empty and unevaluated array bounds
  are accepted only where the type can be completed by context.

## Accepted expressions and statements

All ordinary C17 operators are tokenized and the core scalar operators are
type-checked. Declarations, expressions, function calls, member access,
subscripting, `if`, `else`, `while`, `do`, `for`, `switch`, `case`, `default`,
`break`, `continue`, `return`, `goto`, labels, and compound statements are
represented in the typed AST. Integer promotions and usual arithmetic
conversions are explicit in the type system.

## Integer constant expressions

Array bounds, enumerator values, case labels, and the operands of the
preprocessor's `#if` are integer constant expressions. The front end evaluates
them with a project evaluator over integer literals, character constants,
enumeration constants, `sizeof`, casts, unary operators, binary operators, and
the conditional operator. A value that cannot be proven constant is rejected
with a diagnostic rather than folded.

## Floating constants

A decimal floating constant is converted by project code, never by a library
routine, so that a bootstrap build and a self-hosted build convert the same
text to the same bits. The conversion forms the significand exactly, applies
the decimal exponent in a 128-bit intermediate, and rounds once to the nearest
binary64 value with ties to even; an `f` suffix narrows the result to binary32
with the same rounding rule. The supported range is at most 19 significant
decimal digits, a decimal exponent between -27 and 19, and a result in the
normal binary64 range. A constant outside that range is diagnosed as an
invalid floating constant instead of being rounded approximately.
Hexadecimal floating constants are not part of this subset.

## Explicitly deferred diagnostics

The following produce stable semantic diagnostics in version 1: variable
length arrays, bit-fields, `_Atomic`, `_Alignas`, generic selection, complex
and imaginary types, `long double`, thread-local storage, compound literals,
anonymous aggregates, flexible aggregate members, and dynamic libraries.
Aggregate assignment and basic binary32/binary64 arithmetic are implemented
for the version 1 integer ABI. Variadic calls are implemented for integer and
pointer arguments, as recorded in the ABI document. Full floating-point
conversions, floating variadic arguments, and aggregate-by-value parameter
passing remain deferred and are diagnosed or kept outside the first target
gate.

A construct outside this document is not accepted by silently extending the
grammar. It must receive a diagnostic before code generation.
