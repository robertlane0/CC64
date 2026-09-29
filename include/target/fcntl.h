#ifndef CC64_TARGET_FCNTL_H
#define CC64_TARGET_FCNTL_H

/* The target opens a file for reading, for writing, or for both, and the mode
   is the one the documented service takes. There is no descriptor flag query
   and no non-blocking or append behaviour, so the access-mode constants are
   all the interface needs. */

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
/* The three access modes occupy the low bits, so a request can ask for one
   of them without naming which. */
#define O_ACCMODE 0x0003
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
#define O_EXCL   0x0800
#define O_BINARY 0x1000
/* The target has one program and no descriptor table to leak into, so a
   request to close a descriptor on execution has nothing to do. The name is
   present so a caller that composes it reads as the interface it wrote, and
   it changes nothing about how the file is opened. */
#define O_CLOEXEC 0x2000
#define O_NOCTTY 0x4000
#define O_SYNC 0x4010
/* The target has no way to ask for a read that waits, so a request for
   one is accepted and the read behaves as an ordinary one. */
#define O_NONBLOCK 0x8000

#define F_GETFL 3
#define F_SETFL 4

/* The target records a file's access mode when it is opened and offers no way
   to change it afterwards, so a query reports what was asked for and a change
   reports that there was nothing to change. */
int fcntl(int handle, unsigned int request, ...);

#endif
