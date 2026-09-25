#ifndef CC64_TARGET_STDARG_H
#define CC64_TARGET_STDARG_H

#include <stddef.h>

/* Version 1 variadic contract: unnamed arguments are integer or pointer
   values passed in the six integer argument registers. A variadic function
   spills those registers in its prologue; __cc64_va_start returns the first
   unnamed slot. Every slot is a whole register, so va_arg always advances by
   eight bytes and narrows the loaded value to the requested type. Floating
   variadic arguments are not part of this contract. */
typedef char *va_list;

void *__cc64_va_start(void);

#define va_start(arguments, last) ((arguments) = (char *)__cc64_va_start())
#define va_arg(arguments, type) (*(type *)(((arguments) += 8) - 8))
#define va_end(arguments) ((void)(arguments))
#define va_copy(destination, source) ((destination) = (source))

#endif
