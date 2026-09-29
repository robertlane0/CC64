#ifndef CC64_TARGET_FCNTL_H
#define CC64_TARGET_FCNTL_H

/* The target opens a file for reading, for writing, or for both, and the mode
   is the one the documented service takes. There is no descriptor flag query
   and no non-blocking or append behaviour, so the access-mode constants are
   all the interface needs. */

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
#define O_EXCL   0x0800
#define O_BINARY 0x1000

#define F_GETFL 3
#define F_SETFL 4

#endif
