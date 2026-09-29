#ifndef CC64_TARGET_SYS_MMAN_H
#define CC64_TARGET_SYS_MMAN_H

#include <stddef.h>

/* The target has no memory mapping service: memory comes from the target's
   allocator, which returns a block of the requested size or nothing. A mapping
   is therefore the allocation with its protection ignored, and the protection
   and flag names are the portable values so a caller that composes them still
   reads as the interface it wrote. */

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_PRIVATE 0x0002
#define MAP_SHARED  0x0001
#define MAP_ANONYMOUS 0x0020
#define MAP_FIXED  0x0010

#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL 0
#define MADV_DONTNEED 4

void *mmap(void *address, size_t length, int protect, int flags, int handle,
           long offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protect);
int madvise(void *address, size_t length, int advice);

#endif
