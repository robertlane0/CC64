#ifndef CC64_TARGET_SYS_WAIT_H
#define CC64_TARGET_SYS_WAIT_H

#include <sys/types.h.h>

/* The target runs one program and has no process to wait for, so the wait
   interface is present with its portable names and reports that there is
   nothing to wait for. */

#define WNOHANG 1
#define WUNTRACED 2
#define WIFEXITED(status) 1
#define WEXITSTATUS(status) ((status) >> 8)
#define WIFSIGNALED(status) 0
#define WTERMSIG(status) ((status) & 0x7F)

pid_t wait(int *status);
pid_t waitpid(pid_t process, int *status, int options);

#endif
