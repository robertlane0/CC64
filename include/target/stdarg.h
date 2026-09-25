#ifndef CC64_TARGET_STDARG_H
#define CC64_TARGET_STDARG_H

typedef char *va_list;
#define va_start(arguments, last) ((arguments) = (va_list)0)
#define va_arg(arguments, type) (*(type *)((arguments)))
#define va_end(arguments) ((void)(arguments))
#define va_copy(destination, source) ((destination) = (source))

#endif
