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

void *cc64_xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size == 0U ? 1U : size);
    if (next == NULL) {
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
