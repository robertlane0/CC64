#ifndef CC64_TARGET_SYS_TYPES_H
#define CC64_TARGET_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* The target's own file and process types, with the portable names a caller
   uses. There is no process, so the process id is the value the target
   reports for the one program it is running. */

typedef long off_t;
typedef long off64_t;
typedef long ssize_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int dev_t;
typedef unsigned int ino_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef long time_t;

#endif
