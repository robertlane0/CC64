#ifndef CC64_TARGET_SYS_IOCTL_H
#define CC64_TARGET_SYS_IOCTL_H

/* The target console is a serial line with a fixed character size, so the
   queries a terminal program makes are answered from constants rather than
   from a device. The request names are the portable ones, so a caller that
   composes them reads as the interface it wrote. */

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGWINSZ_ 0x40087468
#define FIONREAD   0x541B

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int handle, unsigned long request, void *argument);

#endif
