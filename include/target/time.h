#ifndef CC64_TARGET_TIME_H
#define CC64_TARGET_TIME_H

#include <stddef.h>
#include <stdint.h>

/* The target has one clock: the tick count the timer service reports since the
   machine started. It is monotonic and has no calendar interpretation, so the
   calendar fields of a POSIX time value are zero and the elapsed fields are
   filled from the tick count. */

typedef long time_t;
typedef long clock_t;
typedef int clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define CLOCKS_PER_SEC 1000

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

int clock_gettime(clockid_t clock, struct timespec *value);
int gettimeofday(struct timeval *value, void *zone);
time_t time(time_t *value);
clock_t clock(void);

#endif
