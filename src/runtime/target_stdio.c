/* Target streams for MS-DOS64.
 *
 * A stream is an open target handle plus the flags the C interface needs.
 * Output is unbuffered: every write goes straight to the target service, which
 * keeps a self-hosted compiler's diagnostics in order even after a long run and
 * keeps the implementation small enough to audit. Input is unbuffered too, so
 * a partial read is visible to the caller as a short count. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cc64_create(const char *path, int mode);
int cc64_open(const char *path);
int cc64_read(int handle, void *data, unsigned long size);
int cc64_write(int handle, const void *data, unsigned long size);
int cc64_close(int handle);
int cc64_lseek(int handle, long offset, int origin);
int cc64_putc(int character);

struct cc64_file {
    int handle;
    int flags;
    long position;
};

#define CC64_STREAM_OPEN 1
#define CC64_STREAM_ERROR 2
#define CC64_STREAM_EOF 4
#define CC64_STREAM_WRITABLE 8

static struct cc64_file standard_input = {0, CC64_STREAM_OPEN, 0L};
static struct cc64_file standard_output = {1, CC64_STREAM_OPEN | CC64_STREAM_WRITABLE, 0L};
static struct cc64_file standard_error = {2, CC64_STREAM_OPEN | CC64_STREAM_WRITABLE, 0L};

FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

int errno;

FILE *fopen(const char *path, const char *mode)
{
    int writable = mode[0] == 'w' || mode[0] == 'a';
    int handle = writable ? cc64_create(path, 0) : cc64_open(path);
    if (handle < 0) {
        errno = 2;
        return NULL;
    }
    FILE *stream = (FILE *)malloc(sizeof(struct cc64_file));
    if (stream == NULL) {
        (void)cc64_close(handle);
        return NULL;
    }
    struct cc64_file *file = (struct cc64_file *)stream;
    file->handle = handle;
    file->position = 0L;
    file->flags = CC64_STREAM_OPEN;
    if (writable) file->flags |= CC64_STREAM_WRITABLE;
    return stream;
}

int fclose(FILE *stream)
{
    if (stream == NULL) return 0;
    struct cc64_file *file = (struct cc64_file *)stream;
    int result = 0;
    if ((file->flags & CC64_STREAM_OPEN) != 0 &&
        (file->flags & CC64_STREAM_WRITABLE) != 0) {
        if (cc64_close(file->handle) < 0) result = -1;
    }
    file->flags = 0;
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return result;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    if (stream == NULL || size == 0U || count == 0U) return 0U;
    struct cc64_file *file = (struct cc64_file *)stream;
    if ((file->flags & CC64_STREAM_OPEN) == 0) {
        file->flags |= CC64_STREAM_ERROR;
        return 0U;
    }
    size_t want = size * count;
    int moved = cc64_read(file->handle, buffer, (unsigned long)want);
    if (moved <= 0) {
        file->flags |= CC64_STREAM_EOF;
        return 0U;
    }
    file->position += (long)moved;
    return (size_t)moved / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    if (stream == NULL || size == 0U || count == 0U) return 0U;
    struct cc64_file *file = (struct cc64_file *)stream;
    if ((file->flags & CC64_STREAM_OPEN) == 0 ||
        (file->flags & CC64_STREAM_WRITABLE) == 0) {
        file->flags |= CC64_STREAM_ERROR;
        return 0U;
    }
    size_t want = size * count;
    int moved = cc64_write(file->handle, buffer, (unsigned long)want);
    if (moved < 0) {
        file->flags |= CC64_STREAM_ERROR;
        return 0U;
    }
    file->position += (long)moved;
    return (size_t)moved / size;
}

int fseek(FILE *stream, long offset, int origin)
{
    if (stream == NULL) return -1;
    struct cc64_file *file = (struct cc64_file *)stream;
    if ((file->flags & CC64_STREAM_OPEN) == 0) return -1;
    long result = cc64_lseek(file->handle, offset, origin);
    if (result < 0L) return -1;
    file->position = result;
    file->flags &= ~CC64_STREAM_EOF;
    return 0;
}

long ftell(FILE *stream)
{
    if (stream == NULL) return -1L;
    struct cc64_file *file = (struct cc64_file *)stream;
    if ((file->flags & CC64_STREAM_OPEN) == 0) return -1L;
    return file->position;
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0L, 0);
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
}

int feof(FILE *stream)
{
    if (stream == NULL) return 1;
    struct cc64_file *file = (struct cc64_file *)stream;
    return (file->flags & CC64_STREAM_EOF) != 0;
}

int ferror(FILE *stream)
{
    if (stream == NULL) return 1;
    struct cc64_file *file = (struct cc64_file *)stream;
    return (file->flags & CC64_STREAM_ERROR) != 0;
}

int fputc(int character, FILE *stream)
{
    unsigned char byte = (unsigned char)character;
    if (fwrite(&byte, 1U, 1U, stream) != 1U) return -1;
    return (int)byte;
}

int fputs(const char *text, FILE *stream)
{
    size_t length = strlen(text);
    if (length == 0U) return 0;
    return fwrite(text, 1U, length, stream) == length ? 0 : -1;
}

int putchar(int character)
{
    return fputc(character, stdout);
}

int puts(const char *text)
{
    if (fputs(text, stdout) != 0) return -1;
    return fputc('\n', stdout) == '\n' ? 0 : -1;
}

int remove(const char *path)
{
    /* The pinned target dispatches no delete-file service, so a file that was
       created cannot be unlinked. Report failure rather than pretending. */
    (void)path;
    errno = 22;
    return -1;
}

int rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    /* No rename service exists on the target; publication writes in place. */
    errno = 22;
    return -1;
}
