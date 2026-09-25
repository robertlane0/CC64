#include "cc64.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGNMENT 16U

struct ArenaBlock {
    ArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char *data;
};

static bool align_size(size_t value, size_t *result)
{
    size_t mask = ARENA_ALIGNMENT - 1U;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

static bool size_add(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool size_mul(size_t left, size_t right, size_t *result)
{
    if (left != 0U && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

Arena *arena_create(size_t limit)
{
    Arena *arena = cc64_xmalloc(sizeof(*arena));
    arena->head = NULL;
    arena->total = 0U;
    arena->limit = limit;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size)
{
    size_t capacity;
    if (!align_size(size == 0U ? 1U : size, &capacity)) {
        return NULL;
    }
    ArenaBlock *block = arena->head;
    size_t total;

    if (!size_add(arena->total, capacity, &total) || total > arena->limit) {
        return NULL;
    }
    if (block == NULL || block->capacity - block->used < capacity) {
        size_t block_capacity = capacity > 65536U ? capacity : 65536U;
        size_t remaining = arena->limit - arena->total;
        if (block_capacity > remaining) {
            block_capacity = remaining;
        }
        if (block_capacity < capacity) {
            return NULL;
        }
        size_t allocation = block_capacity;
        if (!size_add(allocation, sizeof(*block), &allocation)) {
            return NULL;
        }
        block = cc64_xmalloc(sizeof(*block));
        block->data = cc64_xmalloc(block_capacity);
        block->used = 0U;
        block->capacity = block_capacity;
        block->next = arena->head;
        arena->head = block;
    }
    void *result = block->data + block->used;
    block->used += capacity;
    arena->total += capacity;
    return result;
}

void *arena_alloc_array(Arena *arena, size_t count, size_t size)
{
    size_t bytes;
    if (!size_mul(count, size, &bytes)) {
        return NULL;
    }
    return arena_alloc(arena, bytes);
}

void arena_destroy(Arena *arena)
{
    if (arena == NULL) {
        return;
    }
    ArenaBlock *block = arena->head;
    while (block != NULL) {
        ArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    free(arena);
}
