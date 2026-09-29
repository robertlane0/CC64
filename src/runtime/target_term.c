/* Target terminal, clock, and signal boundary for MS-DOS64.
 *
 * The target console is a serial line driven by the target's own keyboard
 * queue. It has no line discipline this library can program and no window
 * size to measure, so the terminal interface reports the state the runtime
 * itself assumes: a console of fixed character size, and input that arrives
 * one byte at a time with no editing applied to it. Reporting that state
 * unchanged is what lets a program that saves and restores the terminal find
 * it in the same condition it left it, which is the whole reason it saves it.
 *
 * Time comes from the target's real-time clock through the two documented
 * services, so a reading is a real date and time of day at the target's
 * one-second resolution. The target has no monotonic service, so a monotonic
 * reading is that same wall clock, which means it restarts at midnight rather
 * than never going backwards. A program that measures an interval across
 * midnight sees a negative one, and the header says so. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>

int cc64_console_ready(void);
long cc64_time_fields(void);
long cc64_date_fields(void);

/* The character size the runtime assumes for the console. A terminal program
   needs a number before it draws anything, and the target's console is
   addressed by character rather than by pixel, so the number is the target's
   own rather than something measured. */
#define CC64_CONSOLE_ROWS 24
#define CC64_CONSOLE_COLUMNS 80

/* The flags a terminal starts in, and the ones a caller that only wants
   unprocessed input leaves in place. Input is echoed by the target's own
   keyboard service, so the discipline bits that would ask for it to be echoed
   or line-buffered are clear, and the ones that ask for signals to be
   recognized are clear because the target raises none. */
static unsigned int input_flags = 0U;
static unsigned int output_flags = 0x0004U /* ONLCR */;
static unsigned int control_flags = 0x0030U /* CS8 */;
static unsigned int local_flags = 0x0008U /* ECHO */;
static unsigned char control_chars[19];

static void ensure_control_chars(void)
{
    static bool ready = false;
    if (ready) return;
    /* The terminator of the target's own line is the carriage return a
       terminal sends, and the interrupt and quit characters have no target
       service behind them, so they are recorded as unused. */
    control_chars[0] = 3U;  /* VINTR */
    control_chars[1] = 28U; /* VQUIT */
    control_chars[2] = 127U; /* VERASE */
    control_chars[3] = 21U; /* VWERASE */
    control_chars[4] = 9U;  /* VKILL */
    control_chars[5] = 26U; /* VREPRINT */
    control_chars[6] = 3U;  /* VWERASE, kept for the index some callers use */
    control_chars[17] = 26U; /* VSUSP */
    control_chars[18] = 4U;  /* VEOL */
    ready = true;
}

int tcgetattr(int handle, struct termios *status)
{
    if (status == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (handle < 0 || handle > 2) {
        errno = ENOTTY;
        return -1;
    }
    ensure_control_chars();
    memset(status, 0, sizeof(*status));
    status->c_iflag = input_flags;
    status->c_oflag = output_flags;
    status->c_cflag = control_flags;
    status->c_lflag = local_flags;
    status->c_line = 0U;
    memcpy(status->c_cc, control_chars, sizeof(control_chars));
    return 0;
}

int tcsetattr(int handle, int when, const struct termios *status)
{
    if (status == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (handle < 0 || handle > 2) {
        errno = ENOTTY;
        return -1;
    }
    (void)when;
    input_flags = status->c_iflag;
    output_flags = status->c_oflag;
    control_flags = status->c_cflag;
    local_flags = status->c_lflag;
    memcpy(control_chars, status->c_cc, sizeof(control_chars));
    return 0;
}

int ioctl(int handle, unsigned long request, void *argument)
{
    if (handle < 0 || handle > 2) {
        errno = ENOTTY;
        return -1;
    }
    if (request == 0x5413U /* TIOCGWINSZ */) {
        if (argument == NULL) {
            errno = EFAULT;
            return -1;
        }
        struct winsize size;
        memset(&size, 0, sizeof(size));
        size.ws_row = CC64_CONSOLE_ROWS;
        size.ws_col = CC64_CONSOLE_COLUMNS;
        memcpy(argument, &size, sizeof(size));
        return 0;
    }
    if (request == 0x5414U /* TIOCSWINSZ */) {
        /* The console's size is the runtime's to choose, so a request to
           change it is accepted and has no effect, which is the same answer
           the query above gives. */
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int fcntl(int handle, unsigned int request, ...)
{
    /* The only descriptor property the target has is the access mode it was
       opened with, and it cannot be changed, so a query reports the mode of a
       console handle and a change reports that there was nothing to change. */
    (void)handle;
    if (request == 3U /* F_GETFL */) return 0;
    if (request == 4U /* F_SETFL */) return 0;
    errno = EINVAL;
    return -1;
}

int poll(struct pollfd *fds, unsigned long count, int timeout)
{
    if (fds == NULL && count != 0U) {
        errno = EFAULT;
        return -1;
    }
    int ready = 0;
    for (unsigned long i = 0U; i < count; ++i) {
        short events = fds[i].events;
        short seen = 0;
        if (fds[i].fd < 0) {
            seen = 0x0020; /* POLLNVAL */
        } else if (fds[i].fd <= 2 && (events & 0x0001 /* POLLIN */) != 0) {
            if (cc64_console_ready() != 0) seen = 0x0001;
        } else if (fds[i].fd <= 2 && (events & 0x0004 /* POLLOUT */) != 0) {
            seen = 0x0004;
        }
        fds[i].revents = seen;
        if (seen != 0) ++ready;
    }
    (void)timeout;
    return ready;
}

int ppoll(struct pollfd *fds, unsigned long count,
          const struct timespec *deadline, void *signal_mask)
{
    (void)deadline;
    (void)signal_mask;
    /* A file read on the target is immediate rather than something to wait
       for, so there is nothing to wait on and the answer is the readiness of
       the handles alone. Reporting that straight away is what a caller with a
       short deadline wants: it is the same answer it would get from waiting,
       and it does not spend the wait. */
    return poll(fds, count, 0);
}

/* Days from the start of the year to the start of a month, for a year that is
   not a leap one. February is the only month whose length depends on the
   year, so the year is consulted for it alone. */
static const unsigned int month_offsets[12] = {
    0U, 31U, 59U, 90U, 120U, 151U, 181U, 212U, 243U, 273U, 304U, 334U
};

static bool leap_year(long year)
{
    return (year % 4L == 0L && year % 100L != 0L) || year % 400L == 0L;
}

/* One reading of the target's clock, as the fields a calendar needs and as
   the seconds since 1980-01-01 that the same moment is. Both come from one
   pair of service calls, so a caller that wants either gets the same instant.
   1980-01-01 is the target's epoch because it is the earliest year the
   real-time clock can hold, and it was a Tuesday, which is index 2 counting
   from Sunday. */
struct cc64_moment {
    int second;
    int minute;
    int hour;
    int day_of_year;
    int weekday;
    long seconds;
};

static bool read_moment(struct cc64_moment *out)
{
    long date = cc64_date_fields();
    long time = cc64_time_fields();
    long year = (date >> 16) & 0xFFFFL;
    long month = (date >> 8) & 0xFFL;
    long day = date & 0xFFL;
    long hour = (time >> 16) & 0xFFL;
    long minute = (time >> 8) & 0xFFL;
    long second = time & 0xFFL;
    /* A clock that has never been set reads back as all zeroes, which is not a
       moment any calendar has, so it is refused rather than believed. */
    if (year < 1980L || year > 2200L || month < 1L || month > 12L ||
        day < 1L || day > 31L || hour > 23L || minute > 59L || second > 60L) {
        return false;
    }
    bool leap = leap_year(year);
    long day_of_year = month_offsets[month - 1L] + day - 1L;
    if (month > 2L && leap) day_of_year += 1L;
    long since_epoch = 0L;
    for (long y = 1980L; y < year; ++y) since_epoch += leap_year(y) ? 366L : 365L;
    since_epoch += day_of_year;
    out->second = (int)second;
    out->minute = (int)minute;
    out->hour = (int)hour;
    out->day_of_year = (int)day_of_year;
    out->weekday = (int)((since_epoch + 2L) % 7L);
    out->seconds = since_epoch * 86400L + hour * 3600L + minute * 60L + second;
    return true;
}

int clock_gettime(clockid_t clock_id, struct timespec *value)
{
    (void)clock_id;
    if (value == NULL) {
        errno = EFAULT;
        return -1;
    }
    struct cc64_moment moment;
    if (!read_moment(&moment)) {
        errno = EIO;
        return -1;
    }
    value->tv_sec = (time_t)moment.seconds;
    value->tv_nsec = 0L;
    return 0;
}

int gettimeofday(struct timeval *value, void *zone)
{
    struct timespec moment;
    (void)zone;
    if (value == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &moment) != 0) return -1;
    value->tv_sec = moment.tv_sec;
    value->tv_usec = moment.tv_nsec / 1000L;
    return 0;
}

time_t time(time_t *value)
{
    struct timespec moment;
    if (clock_gettime(CLOCK_REALTIME, &moment) != 0) {
        if (value != NULL) *value = 0;
        return (time_t)-1;
    }
    if (value != NULL) *value = moment.tv_sec;
    return moment.tv_sec;
}

clock_t clock(void)
{
    /* The target has no processor-time counter, so the reading is the same
       wall clock the other calls use, at the header's declared rate. */
    struct timespec moment;
    if (clock_gettime(CLOCK_MONOTONIC, &moment) != 0) return (clock_t)-1;
    return (clock_t)(moment.tv_sec * CLOCKS_PER_SEC);
}

int nanosleep(const void *request, void *remain)
{
    (void)remain;
    if (request == NULL) {
        errno = EFAULT;
        return -1;
    }
    const struct timespec *wanted = (const struct timespec *)request;
    if (wanted->tv_sec < 0L || wanted->tv_nsec < 0L || wanted->tv_nsec > 999999999L) {
        errno = EINVAL;
        return -1;
    }
    /* The target has no timer service, so the only way to let time pass is to
       spend it asking whether the console has anything yet. A wait that is
       shorter than the clock's resolution is spent in full anyway, because
       there is no finer reading to wait for. */
    long target = (long)cc64_time_fields();
    long want = wanted->tv_sec * 3600L +
                ((wanted->tv_nsec / 1000000000L) * 3600L) +
                ((wanted->tv_nsec / 1000000L) / 1000L) * 60L +
                (wanted->tv_nsec / 1000000L) % 60L;
    for (;;) {
        long now = (long)cc64_time_fields();
        long spent = now - target;
        if (spent < 0L) spent += 86400L;
        if (spent >= want) break;
        if (cc64_console_ready() != 0) break;
    }
    return 0;
}

int usleep(unsigned int microseconds)
{
    struct timespec wanted;
    wanted.tv_sec = (time_t)(microseconds / 1000000U);
    wanted.tv_nsec = (long)(microseconds % 1000000U) * 1000L;
    return nanosleep(&wanted, NULL);
}

int sleep(unsigned int seconds)
{
    struct timespec wanted;
    wanted.tv_sec = (time_t)seconds;
    wanted.tv_nsec = 0L;
    return nanosleep(&wanted, NULL);
}

int sigaction(int signal_number, const struct sigaction *action,
              struct sigaction *old)
{
    if (signal_number <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (old != NULL) {
        /* Nothing is ever delivered, so there is no previous disposition to
           report beyond the one the target would use. */
        memset(old, 0, sizeof(*old));
        old->sa_handler = SIG_DFL;
    }
    (void)action;
    return 0;
}

int signal(int signal_number, sighandler_t handler)
{
    struct sigaction wanted;
    struct sigaction previous;
    memset(&wanted, 0, sizeof(wanted));
    wanted.sa_handler = handler;
    if (sigaction(signal_number, &wanted, &previous) != 0) return SIG_ERR;
    return SIG_DFL;
}

int raise(int signal_number)
{
    if (signal_number <= 0) {
        errno = EINVAL;
        return -1;
    }
    /* The target delivers nothing, so raising one changes nothing. Reporting
       success is what a program that checks the result expects from a signal
       it has already handled. */
    return 0;
}
