#include "cc64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *cc64_xmalloc(size_t size)
{
    void *ptr = malloc(size == 0U ? 1U : size);
    if (ptr == NULL) {
        fputs("cc64: out of memory\n", stderr);
        exit(2);
    }
    return ptr;
}

void *cc64_xcalloc(size_t count, size_t size)
{
    if (count == 0U) {
        return cc64_xmalloc(1U);
    }
    if (count > SIZE_MAX / (size == 0U ? 1U : size)) {
        fputs("cc64: allocation size overflow\n", stderr);
        exit(2);
    }
    void *ptr = calloc(count, size == 0U ? 1U : size);
    if (ptr == NULL) {
        fputs("cc64: out of memory\n", stderr);
        exit(2);
    }
    return ptr;
}

/* The target allocator reports a refusal as -1, and a host allocator reports it
   as a null pointer, so both spellings have to be recognised here: a refused
   request that was mistaken for a block would be written through. */
static bool allocation_refused(void *pointer)
{
    return pointer == NULL || pointer == (void *)-1;
}

void *cc64_xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size == 0U ? 1U : size);
    if (allocation_refused(next)) {
        fputs("cc64: out of memory\n", stderr);
        exit(2);
    }
    return next;
}

char *cc64_xstrdup(const char *text)
{
    size_t length = strlen(text);
    char *copy = cc64_xmalloc(length + 1U);
    memcpy(copy, text, length + 1U);
    return copy;
}

/* Publication policy (D-009): the caller has already built the complete
   image in memory, so the output is created once and written once. A failed
   compilation never reaches this function, and a truncated write is detected
   by the CC64O/MZ64 checksum when the file is read back. The target service
   contract has no rename or delete service, so no temporary file is used. */
bool cc64_write_file(const char *path, const void *data, size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return false;
    bool good = size == 0U || fwrite(data, 1U, size, stream) == size;
    if (fclose(stream) != 0) good = false;
    return good;
}
