/* Target environment, working directory, and directory reading.
 *
 * The version 1 startup contract builds `argc` and `argv` from the process
 * prefix and hands nothing else to `main`: the target's environment belongs to
 * the shell, and a running program has no documented way to reach the prefix
 * the loader gave it. `getenv` therefore reports that no variable is set, which
 * is a statement about this interface rather than about the target's shell: a
 * program that needs a variable and finds none takes its documented default,
 * which is what a target with no environment for its programs means.
 *
 * The volume has one flat directory and no subdirectories, so the working
 * directory is always the volume root and a directory reading yields nothing.
 * Both are reported as the target's own answers instead of being approximated,
 * because a program that listed a directory here could not act on what it
 * found. */

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

char *getcwd(char *buffer, size_t size)
{
    /* The root is the one directory the target has, and it always exists, so
       the answer never depends on a caller's buffer being big enough for
       anything longer. */
    static const char root[] = "/";
    size_t needed = sizeof(root);
    if (buffer == NULL || size < needed) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, root, needed);
    return buffer;
}

int chdir(const char *path)
{
    /* There is nowhere else to be, so a request for a directory the caller
       named is honoured only when it is the one already in effect. */
    if (path == NULL || strcmp(path, "/") == 0 || strcmp(path, ".") == 0) return 0;
    errno = ENOENT;
    return -1;
}

/* A directory stream is a fixed record rather than a cursor into a listing,
   because the target has no listing for it to hold. The record is handed to
   `opendir` so that a caller that checks the result before reading sees the
   same pointer back, which is what the interface promises. */
struct cc64_dir {
    int served;
};

static struct cc64_dir the_directory;

DIR *opendir(const char *path)
{
    if (path == NULL) {
        errno = ENOENT;
        return NULL;
    }
    /* A name that is not the root names something the target cannot list. The
       root is the one directory, and it lists as empty. */
    if (strcmp(path, "/") != 0 && strcmp(path, ".") != 0 &&
        strcmp(path, "./") != 0) {
        errno = ENOTDIR;
        return NULL;
    }
    the_directory.served = 0;
    return &the_directory;
}

struct dirent *readdir(DIR *stream)
{
    if (stream == NULL) {
        errno = EBADF;
        return NULL;
    }
    if (stream->served != 0) return NULL;
    stream->served = 1;
    return NULL;
}

int closedir(DIR *stream)
{
    if (stream == NULL) {
        errno = EBADF;
        return -1;
    }
    stream->served = 1;
    return 0;
}

void rewinddir(DIR *stream)
{
    if (stream != NULL) stream->served = 0;
}

int dirfd(DIR *stream)
{
    return stream == NULL ? -1 : 0;
}
