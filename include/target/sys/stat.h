#ifndef CC64_TARGET_SYS_STAT_H
#define CC64_TARGET_SYS_STAT_H

#include <stddef.h>
#include <stdint.h>

/* A file's size and kind are all the target reports. The mode and the owner
   fields a POSIX structure carries have no meaning here, so they are present
   with their portable values rather than omitted, which keeps a caller's
   structure layout the one it declares. */

#define S_IFREG  0x8000
#define S_IFDIR  0x4000

struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
};

int stat(const char *path, struct stat *status);
int lstat(const char *path, struct stat *status);
int fstat(int handle, struct stat *status);
int mkdir(const char *path, int mode);
int rmdir(const char *path);

#endif
