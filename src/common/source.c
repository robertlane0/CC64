#include "cc64.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

SourceManager *source_manager_create(Arena *arena)
{
    SourceManager *manager = arena_alloc(arena, sizeof(*manager));
    if (manager == NULL) {
        return NULL;
    }
    manager->arena = arena;
    manager->sources = NULL;
    manager->count = 0U;
    manager->capacity = 0U;
    return manager;
}

Source *source_manager_add(SourceManager *manager, const char *path,
                           const unsigned char *bytes, size_t length)
{
    if (manager->count == manager->capacity) {
        size_t next = manager->capacity == 0U ? 8U : manager->capacity * 2U;
        Source **sources = arena_alloc_array(manager->arena, next, sizeof(*sources));
        if (sources == NULL) {
            return NULL;
        }
        if (manager->sources != NULL) {
            memcpy(sources, manager->sources, manager->count * sizeof(*sources));
        }
        manager->sources = sources;
        manager->capacity = next;
    }

    Source *source = arena_alloc(manager->arena, sizeof(*source));
    unsigned char *copy = arena_alloc(manager->arena, length + 1U);
    if (source == NULL || copy == NULL) {
        return NULL;
    }
    if (length != 0U) {
        memcpy(copy, bytes, length);
    }
    copy[length] = 0U;
    source->path = cc64_xstrdup(path);
    source->bytes = copy;
    source->length = length;
    source->id = (unsigned)manager->count + 1U;
    manager->sources[manager->count++] = source;
    return source;
}

Source *source_manager_load(SourceManager *manager, const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return NULL;
    }
    if (fseek(stream, 0L, SEEK_END) != 0) {
        fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    size_t length = (size_t)end;
    unsigned char *bytes = arena_alloc(manager->arena, length + 1U);
    if (bytes == NULL) {
        fclose(stream);
        return NULL;
    }
    if (length != 0U && fread(bytes, 1U, length, stream) != length) {
        fclose(stream);
        return NULL;
    }
    if (fclose(stream) != 0) {
        return NULL;
    }
    return source_manager_add(manager, path, bytes, length);
}

const Source *source_by_id(const SourceManager *manager, unsigned id)
{
    if (id == 0U || id > manager->count) {
        return NULL;
    }
    return manager->sources[id - 1U];
}
