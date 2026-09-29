#ifndef CC64_TARGET_UNISTD_H
#define CC64_TARGET_UNISTD_H

#include <stddef.h>
#include <stdint.h>

/* A count of bytes a read or a write can report, and a position a seek can
   report, are both as wide as the machine's own counters. */
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;

/* The POSIX names the target's freestanding library provides, on the target's
   own file-handle and console model. A handle is the small non-negative value
   the target service returned; a stream is the same value until it is closed.
   There is no process, no file descriptor table beyond the target's, and no
   directory hierarchy, so a call that cannot be satisfied returns -1 and sets
   errno rather than reporting a host error. */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

int open(const char *path, int flags, ...);
int close(int handle);
ssize_t read(int handle, void *data, size_t size);
ssize_t write(int handle, const void *data, size_t size);
int unlink(const char *path);
int access(const char *path, int mode);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
int isatty(int handle);
off_t lseek(int handle, off_t offset, int origin);
int stat(const char *path, void *status);
int fstat(int handle, void *status);
int fsync(int handle);
int truncate(const char *path, off_t length);
int getpid(void);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);
int nanosleep(const void *request, void *remain);
int dup2(int from, int to);
int pipe(int *ends);
char *readlink(const char *path, char *buffer, size_t size);
int rmdir(const char *path);

#endif
