#ifndef CC64_DOS64_H
#define CC64_DOS64_H

#include <stdint.h>
#include <stddef.h>

typedef int cc64_fd;

#define CC64_STDIN 0
#define CC64_STDOUT 1
#define CC64_STDERR 2
#define CC64_SEEK_SET 0
#define CC64_SEEK_CUR 1
#define CC64_SEEK_END 2

void cc64_exit(int code);
int cc64_putc(int character);
int cc64_write(int handle, const void *data, size_t size);
int cc64_read(int handle, void *data, size_t size);
void *cc64_alloc(size_t size);
void cc64_free(void *pointer);
cc64_fd cc64_open(const char *name);
int cc64_close(cc64_fd handle);

#endif
