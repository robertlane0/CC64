#ifndef CC64_TARGET_SIGNAL_H
#define CC64_TARGET_SIGNAL_H

/* The target has one program and the console is a serial line, so a resize is
   not delivered as a signal: a caller that installs a handler for one still
   runs, and the handler is simply never called. The names are present so a
   program that refers to them compiles, and raising one does nothing. */

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIG_ERR ((sighandler_t)-1)
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGWINCH 28

struct sigaction {
    sighandler_t sa_handler;
    sig_atomic_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

int sigaction(int signal, const struct sigaction *action, struct sigaction *old);
int signal(int signal, sighandler_t handler);
int raise(int signal);

#endif
