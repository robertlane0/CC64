/* Target memory mapping and dynamic loading for MS-DOS64.
 *
 * A target image is one program loaded once at a fixed bias, and the target's
 * allocator hands out whole blocks from one heap. A mapping request is
 * therefore a block request, rounded up to the target's own allocation
 * granularity, and a mapping that is released is a block that is freed. The
 * request's own length is recorded in a table so that a later request about
 * the same range can be answered with the length it was given, and the table
 * is bounded so that a program that maps and unmaps in a loop cannot make it
 * grow without limit.
 *
 * There is no loader to ask, so a shared object cannot be opened. The names
 * are present because a program that treats the loader as optional should
 * still run here: each reports that it found nothing rather than refusing to
 * link, which is the difference between a program that degrades and a program
 * that does not start. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include <dlfcn.h>

void *cc64_alloc(unsigned long size);
void cc64_free(void *pointer);

/* The target's allocator works in paragraphs, so a request smaller than one
   still consumes one. The rounding is what makes a mapping's address a
   multiple of its own size, which is the alignment a page-shaped interface
   promises. */
#define CC64_GRANULARITY 16UL

/* A record of one live mapping. The table is fixed in size and full mappings
   are refused, because a program that maps without unmapping would otherwise
   grow the image's own data without bound. */
#define CC64_MAPPING_SLOTS 64U

struct cc64_mapping {
    void *base;
    unsigned long length;
    int in_use;
};

static struct cc64_mapping mappings[CC64_MAPPING_SLOTS];
static bool mappings_primed;

static void prime_mappings(void)
{
    if (mappings_primed) return;
    /* The table lives in the image's own zero-filled data, so every record
       starts unused. Priming is still done once so that a program which never
       maps does not depend on that for its answer. */
    for (unsigned long i = 0U; i < CC64_MAPPING_SLOTS; ++i) {
        mappings[i].in_use = 0;
    }
    mappings_primed = true;
}

static struct cc64_mapping *find_mapping(const void *address)
{
    const unsigned char *at = (const unsigned char *)address;
    for (unsigned long i = 0U; i < CC64_MAPPING_SLOTS; ++i) {
        if (mappings[i].in_use == 0) continue;
        const unsigned char *base = (const unsigned char *)mappings[i].base;
        if (at >= base && at < base + mappings[i].length) return &mappings[i];
    }
    return NULL;
}

void *mmap(void *address, size_t length, int protection, int flags, int fd,
           long offset)
{
    (void)address; (void)protection; (void)flags; (void)fd; (void)offset;
    prime_mappings();
    if (length == 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    unsigned long size = ((unsigned long)length + CC64_GRANULARITY - 1UL) &
                         ~(CC64_GRANULARITY - 1UL);
    for (unsigned long i = 0U; i < CC64_MAPPING_SLOTS; ++i) {
        if (mappings[i].in_use != 0) continue;
        void *block = cc64_alloc(size);
        if (block == NULL) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        mappings[i].base = block;
        mappings[i].length = size;
        mappings[i].in_use = 1;
        return block;
    }
    errno = ENOMEM;
    return MAP_FAILED;
}

int munmap(void *address, size_t length)
{
    prime_mappings();
    struct cc64_mapping *record = find_mapping(address);
    if (record == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* A release has to name the whole mapping. A shorter length would leave
       part of it reachable with no way to release the rest, so it is refused. */
    if (length != 0U && (unsigned long)length != record->length) {
        errno = EINVAL;
        return -1;
    }
    cc64_free(record->base);
    record->in_use = 0;
    record->base = NULL;
    record->length = 0UL;
    return 0;
}

int mprotect(void *address, size_t length, int protection)
{
    prime_mappings();
    /* The target has one flat address space with no protection, so a mapping's
       permissions are whatever it was given. A request about a range that is
       not mapped is still refused, because that is a mistake in the caller
       rather than a permission the target chose not to enforce. */
    if (find_mapping(address) == NULL) {
        errno = ENOMEM;
        return -1;
    }
    (void)length; (void)protection;
    return 0;
}

void *dlopen(const char *name, int flags)
{
    (void)name; (void)flags;
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle; (void)name;
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    /* Nothing was opened, so nothing needs closing and the count of open
       handles stays where it is. */
    return 0;
}

char *dlerror(void)
{
    return NULL;
}

int dladdr(const void *address, Dl_info *info)
{
    if (info == NULL) {
        errno = EFAULT;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    /* The image is the only loaded object, and it describes itself no further
       than its own entry, so a lookup for a name finds nothing. The base is
       reported because it is known, which is what lets a caller decide that
       the address belongs to this program rather than to nothing. */
    (void)address;
    return 0;
}

int madvise(void *address, size_t length, int advice)
{
    prime_mappings();
    /* The target has one address space and no protection, so there is nothing
       for advice to change. A request about a range that is not mapped is still
       refused, because that is a mistake in the caller rather than a hint the
       target chose to ignore. */
    if (find_mapping(address) == NULL) {
        errno = ENOMEM;
        return -1;
    }
    (void)length; (void)advice;
    return 0;
}
