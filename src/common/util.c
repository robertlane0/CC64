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
