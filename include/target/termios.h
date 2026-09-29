#ifndef CC64_TARGET_TERMIOS_H
#define CC64_TARGET_TERMIOS_H

/* The target console is a serial line with no line discipline this library can
   program, so the terminal interface is present with the shape a caller
   declares and reports that the target has no such state to save or set. A
   caller that only wants to know whether a handle is a console asks isatty. */

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define IGNBRK   0x0001
#define BRKINT   0x0002
#define PARMRK   0x0004
#define INPCK    0x0008
#define ISTRIP   0x0010
#define INLCR    0x0020
#define IGNCR    0x0040
#define ICRNL    0x0080
#define IXON     0x0400
#define IXOFF    0x1000
#define IMAXBEL  0x2000
#define IUTF8    0x4000
/* What the line does to the characters that reach it, and what it does with
   the characters it produces. The target console has no line discipline to
   program, so these are present for a caller that composes them and a query
   reports the recorded settings unchanged. */
#define ISIG     0x0001
#define ICANON   0x0002
#define ECHO     0x0008
#define ECHOE    0x0010
#define ECHOK    0x0020
#define ECHONL   0x0040
#define NOFLSH   0x0080
#define TOSTOP   0x0100
#define IEXTEN   0x0200
#define ECHOCTL  0x0800
#define ECHOPRT  0x1000
#define ECHOKE   0x2000
#define FLUSHO   0x4000
#define PENDIN   0x8000
#define EXTPROC  0x0002

#define OPOST    0x0001
#define ONLCR    0x0004
#define OCSANOW  0x0010

#define CSIZE    0x0030
#define CS8      0x0030
#define PARENB   0x0100
#define PARODD   0x0200

#define VMIN     0
#define VTIME    1

#define NCCS     19

struct termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
};

int tcgetattr(int handle, struct termios *status);
int tcsetattr(int handle, int when, const struct termios *status);
int isatty(int handle);

#endif
