/* Target file handles for MS-DOS64, on the documented service boundary.
 *
 * The target has no descriptor table of its own to manage: a handle is the
 * small value a service returned, and every routine here is a thin rename of
 * one service. What the target cannot do is reported as a refusal rather than
 * invented: a path it cannot reach has no way to be created, a link has no way
 * to be followed, and a directory has no way to be listed. Each of those sets
 * `errno` to the value that names the missing capability, so a program that
 * checks the result sees why rather than a wrong success. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cc64_open(const char *name);
int cc64_create(const char *path, int mode);
int cc64_read(int handle, void *data, unsigned long size);
int cc64_write(int handle, const void *data, unsigned long size);
int cc64_close(int handle);
int cc64_lseek(int handle, long offset, int origin);
int cc64_delete(const void *fcb);

/* A file control block is the target's own directory-entry descriptor: one
   byte of drive, then eleven bytes of name, then the extent fields. Only the
   first thirteen are ever read, so the record is sized for them. */
#define CC64_FCB_NAME 11U
#define CC64_FCB_SIZE 64U

/* A handle is a signed value in the target's own range, and a service reports
   a refusal as a negative result, so every count and position below is
   carried as a long and narrowed only on the way in. */
static int fail(int code)
{
    errno = code;
    return -1;
}

static int size_of(int handle, long *out)
{
    long end = (long)cc64_lseek(handle, 0L, 2 /* SEEK_END */);
    if (end < 0L) return fail(errno != 0 ? errno : EIO);
    long back = (long)cc64_lseek(handle, 0L, 0 /* SEEK_SET */);
    if (back < 0L) return fail(errno != 0 ? errno : EIO);
    *out = end;
    return 0;
}

int close(int handle)
{
    int result = cc64_close(handle);
    if (result < 0) return fail(EBADF);
    return 0;
}

ssize_t read(int handle, void *data, size_t size)
{
    if (size == 0U) return 0;
    int moved = cc64_read(handle, data, (unsigned long)size);
    if (moved < 0) return fail(errno != 0 ? errno : EIO);
    return (ssize_t)moved;
}

ssize_t write(int handle, const void *data, size_t size)
{
    if (size == 0U) return 0;
    int put = cc64_write(handle, data, (unsigned long)size);
    if (put < 0) return fail(errno != 0 ? errno : EIO);
    return (ssize_t)put;
}

off_t lseek(int handle, off_t offset, int origin)
{
    long moved = (long)cc64_lseek(handle, (long)offset, origin);
    if (moved < 0L) return (off_t)fail(ESPIPE);
    return (off_t)moved;
}

int isatty(int handle)
{
    /* The target has exactly three console handles, and a program that asks
       about anything else has a file rather than a terminal. */
    return handle >= 0 && handle <= 2 ? 1 : 0;
}

int fsync(int handle)
{
    /* A write that returned has already reached the target's buffer, and the
       target offers no further flush service, so there is nothing to wait for
       and nothing to report. */
    (void)handle;
    return 0;
}

int getpid(void)
{
    /* One program runs at a time, so the calling program is the first one and
       the identifier is the one the target gives a process. */
    return 1;
}

/* The access mode a request asks for, expressed in the target's own three
   ways of opening a file. Only the low bits of the request matter, because
   the target has no descriptor flags. */
static int wanted_mode(int flags)
{
    if ((flags & O_ACCMODE) == O_RDWR) return 2;
    if ((flags & O_ACCMODE) == O_WRONLY) return 1;
    return 0;
}

int open(const char *path, int flags, ...)
{
    int mode = wanted_mode(flags);
    int handle = cc64_open(path);
    if (handle >= 0) {
        if (mode == 2) {
            /* A request to read and write needs a handle opened for both, and
               the target cannot widen one that is already open, so the read
               handle is replaced by a read-and-write one. */
            cc64_close(handle);
            handle = cc64_create(path, 2);
        } else if (mode == 1 || (flags & O_TRUNC) != 0 || (flags & O_CREAT) != 0) {
            cc64_close(handle);
            handle = cc64_create(path, mode);
        }
    } else if ((flags & O_CREAT) != 0) {
        handle = cc64_create(path, mode);
    }
    if (handle < 0) return fail((flags & O_CREAT) != 0 ? ENOENT : EACCES);
    return handle;
}

/* The name a delete service is given is a file control block rather than a
   path, so the path is folded into the target's own fixed-width name field.
   A name longer than the field cannot be expressed, which is reported rather
   than truncated, because a truncated name would delete a different file. */
static int block_for(const char *path, unsigned char *block)
{
    size_t length = strlen(path);
    if (length == 0U || length > CC64_FCB_NAME) return fail(ENOENT);
    memset(block, 0, CC64_FCB_SIZE);
    block[0] = (unsigned char)(path[0] == '/' ? 0U : (unsigned char)(path[0] - 'a' + 1));
    size_t at = 1U;
    size_t index = 1U;
    if (path[0] == '/') ++index;
    bool in_extension = false;
    for (; index < length; ++index) {
        char c = path[index];
        if (c == '.') {
            in_extension = true;
            continue;
        }
        if (c == '/' || c == '\\' || c == ':') return fail(ENOENT);
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'a' + 'A');
        else if (!in_extension && c >= '0' && c <= '9') c = (char)(c - '0' + 1);
        else if (in_extension && c >= '0' && c <= '9') c = (char)(c - '0' + 1);
        else return fail(ENOENT);
        if (at >= CC64_FCB_NAME) return fail(ENOENT);
        block[at++] = (unsigned char)c;
    }
    return 0;
}

int unlink(const char *path)
{
    unsigned char block[CC64_FCB_SIZE];
    if (block_for(path, block) != 0) return -1;
    int result = cc64_delete(block);
    if (result < 0) return fail(ENOENT);
    return 0;
}

int access(const char *path, int mode)
{
    (void)mode;
    int handle = cc64_open(path);
    if (handle < 0) return fail(ENOENT);
    cc64_close(handle);
    return 0;
}

int truncate(const char *path, off_t length)
{
    int handle = cc64_create(path, 1);
    if (handle < 0) return fail(ENOENT);
    long moved = (long)cc64_lseek(handle, (long)length, 0 /* SEEK_SET */);
    cc64_close(handle);
    return moved < 0L ? fail(EIO) : 0;
}

int dup2(int from, int to)
{
    /* The target hands out handles itself and has no table to insert into, so
       there is no second name for an existing handle. A request to put one at
       a chosen number is refused rather than answered with a handle that does
       not mean what the caller asked for. */
    (void)from; (void)to;
    return fail(EMFILE);
}

int pipe(int *ends)
{
    (void)ends;
    return fail(ENOSYS);
}

char *readlink(const char *path, char *buffer, size_t size)
{
    (void)path; (void)buffer; (void)size;
    errno = EINVAL;
    return NULL;
}

int rmdir(const char *path)
{
    (void)path;
    return fail(ENOTDIR);
}

int mkdir(const char *path, int mode)
{
    (void)path; (void)mode;
    return fail(EEXIST);
}

/* A file's status is the two things the target can answer without a
   directory service: that the name reaches a file, and how many bytes the
   file holds. A target file is always a regular one, because the volume has
   no subdirectories for the kind to be anything else. */
static void fill_status(struct stat *status, long length)
{
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFREG;
    status->st_nlink = 1U;
    status->st_size = (unsigned long)length;
    status->st_blksize = 512UL;
    status->st_blocks = (unsigned long)((length + 511L) / 512L);
}

int fstat(int handle, struct stat *status)
{
    long length = 0L;
    if (size_of(handle, &length) != 0) return -1;
    fill_status(status, length);
    return 0;
}

int stat(const char *path, struct stat *status)
{
    int handle = cc64_open(path);
    if (handle < 0) return fail(ENOENT);
    long length = 0L;
    int result = size_of(handle, &length);
    cc64_close(handle);
    if (result != 0) return -1;
    fill_status(status, length);
    return 0;
}

int lstat(const char *path, struct stat *status)
{
    /* The target's volume has no symbolic links, so a name that reaches
       anything reaches that thing itself. */
    return stat(path, status);
}
