/* Target heap for MS-DOS64.
 *
 * The target allocator hands back a whole DOS block, so this file adds a small
 * size header to every allocation. That header is what makes realloc possible:
 * the target has no query for an existing block's size. The returned pointer is
 * the block plus the header, which keeps the sixteen-byte alignment that the
 * compiler's arena allocator depends on, and free subtracts the same header. */

#include <stddef.h>
#include <string.h>

void *cc64_alloc(unsigned long size);
void cc64_free(void *pointer);
void cc64_exit(int code);

#define CC64_HEAP_HEADER 16

void *malloc(size_t size)
{
    if (size == 0U) size = 1U;
    if (size > (size_t)-1 - CC64_HEAP_HEADER) return NULL;
    void *base = cc64_alloc((unsigned long)(size + CC64_HEAP_HEADER));
    if (base == NULL) return NULL;
    *(size_t *)base = size;
    return (char *)base + CC64_HEAP_HEADER;
}

void free(void *pointer)
{
    if (pointer == NULL) return;
    cc64_free((char *)pointer - CC64_HEAP_HEADER);
}

void *calloc(size_t count, size_t size)
{
    if (count != 0U && size > (size_t)-1 / count) return NULL;
    size_t total = count * size;
    void *pointer = malloc(total);
    if (pointer != NULL) memset(pointer, 0, total);
    return pointer;
}

void *realloc(void *pointer, size_t size)
{
    if (pointer == NULL) return malloc(size);
    if (size == 0U) {
        free(pointer);
        return NULL;
    }
    size_t previous = *(size_t *)((char *)pointer - CC64_HEAP_HEADER);
    if (size <= previous) return pointer;
    void *next = malloc(size);
    if (next == NULL) return NULL;
    memcpy(next, pointer, previous);
    free(pointer);
    return next;
}

void exit(int status)
{
    cc64_exit(status);
}

void abort(void)
{
    cc64_exit(3);
}
