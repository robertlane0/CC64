#ifndef CC64_TARGET_POLL_H
#define CC64_TARGET_POLL_H

/* A poll on the target is a wait on at most one console, because the target
   has one console and a bounded set of file handles, and reading a file is
   immediate rather than something to wait for. The shape a caller declares is
   the one it passes, so a program written against this interface runs here
   unchanged. */

#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct pollfd_ {
    int fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, unsigned long count, int timeout);
/* The wait can be given a deadline rather than a number of seconds to
   wait, and the remainder after an early return says how much of the
   deadline was left. */
struct timespec;
int ppoll(struct pollfd *fds, unsigned long count,
          const struct timespec *deadline, void *signal_mask);

#endif
