#ifndef CC64_TARGET_STDIO_H
#define CC64_TARGET_STDIO_H

#include <stddef.h>

typedef struct cc64_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fflush(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fputc(int character, FILE *stream);
int fputs(const char *text, FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int putchar(int character);
int puts(const char *text);
int snprintf(char *buffer, size_t capacity, const char *format, ...);
int vsnprintf(char *buffer, size_t capacity, const char *format, void *args);
int rename(const char *from, const char *to);
int remove(const char *path);

#endif
