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
- `static_assert` and `_Static_assert` at file scope and in block scope, with
  an optional message. The condition is an integer constant expression and is
  checked while the unit is parsed; the assertion emits no code.
- `__func__`, a `static const char` array holding the name of the function
  being parsed. Each function gets its own object.
- `_Alignof(type)`, which yields the alignment the ABI gives the type.
- `__uint128_t`, an unsigned integer type sixteen bytes wide with sixteen-byte
  alignment. It is the widest type the target has. A value of it is two
  general-register eightbytes when it is passed or returned, the same placement
  a record of two eightbytes holding integers receives, and every operation on
  it works in frame storage. Its operations are `*`, `/`, `%`, `+`, `-`, `<<`,
  `>>`, `&`, `|`, `^`, `~`, unary `-`, comparisons, conversions to and from the
  narrower integer types, and assignment. The shift count must be a constant,
  because a machine shift count is six bits wide; a variable count is diagnosed
  with CC3009 rather than reduced.

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
with the same rounding rule. The conversion accepts at most 19 significant
decimal digits, which is what a 64-bit significand holds, and a decimal
exponent over the whole range of the format.

The exponent is applied one table entry at a time, with the significand carried
in 128 bits and brought back to a word between entries so that the next product
has room; the binary scale is adjusted by the amount each step drops. The 53
bits the format keeps sit far above the 128 the working precision carries, so
no bit that decides a rounding is discarded, and every value in the format's
range converts to its correctly rounded encoding.

A constant below the smallest value the format holds rounds to zero, which is a
value the format holds, and is accepted. A constant above the largest has no
value to hold and is diagnosed as an invalid floating constant, because that is
the case where the text is nearly always a mistake. Hexadecimal floating
constants are not part of this subset.

## Compound literals

A type name in parentheses followed by a braced list is a compound literal: an
unnamed object the list initializes, whose lifetime is the enclosing block. It
is a modifiable object rather than a value, so its address can be taken, it can
be assigned through, and it can be indexed and selected from. An array type
written with no length is completed by the list that initializes it, exactly as
it is for a declared object.

A compound literal at file scope would outlive the run, and this revision places
no computed object in the data section, so it is diagnosed with CC2081 rather
than given storage of a kind that does not exist here.

## Explicitly deferred diagnostics

The following produce stable semantic diagnostics in version 1: variable
length arrays, bit-fields, `_Atomic`, `_Alignas`, generic selection, complex
and imaginary types, `long double`, thread-local storage, anonymous aggregates,
flexible aggregate members, and dynamic libraries.
Aggregate assignment, aggregate passing and return by value, and basic
binary32/binary64 arithmetic are implemented for the version 1 ABI. Variadic
calls are implemented for integer and pointer arguments, as recorded in the ABI
document. Full floating-point conversions and floating variadic arguments
remain deferred and are diagnosed or kept outside the first target gate.

Each deferred construct has its own diagnostic identifier, so a program that
uses one is told which construct is out of contract instead of receiving a
generic parse failure and a cascade of follow-on errors.

| Identifier | Construct |
|---:|---|
| CC2010 | variable length array bound |
| CC2018 | bit-field member |
| CC2030 | `_Atomic` or `_Alignas` type specifier |
| CC2037 | `_Complex` or `_Imaginary` type specifier |
| CC2038 | `_Thread_local` storage class |
| CC2039 | `long double` |
| CC2042 | `_Generic` selection |
| CC2081 | compound literal at file scope, or one whose type the list cannot complete |
| CC2016 | anonymous struct or union member |
| CC2033 | `__uint128_t` combined with another type specifier |
| CC3009 | a wide shift whose count is not a constant |

A construct outside this document is not accepted by silently extending the
grammar. It must receive a diagnostic before code generation.
