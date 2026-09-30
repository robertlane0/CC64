/* A program's console input is the keyboard and the line together.
 *
 * The target delivers console input on the line, which is where its own shell
 * reads it, and both emulators put a test's keystrokes there. The services a
 * program uses to receive input have to see the same input, or a program that
 * polls and reads gets nothing while the bytes it was sent sit unread.
 *
 * This asks the two questions a program asks, in the order it asks them: is a
 * character waiting, and then take one. A character sent after the program
 * started must be the one it takes, and taking it twice must not be possible,
 * because a status check that consumed the character would leave the read that
 * followed it with the next one instead.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

int cc64_write(int handle, const void *data, unsigned long size);
int cc64_putc(int character);

static void say(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    cc64_write(1, text, (unsigned long)length);
}

static void put(long value) {
    char digits[24];
    int index = 0;
    unsigned long magnitude = (unsigned long)value;
    if (magnitude == 0) {
        digits[index++] = '0';
    }
    while (magnitude != 0U) {
        digits[index++] = (char)('0' + (int)(magnitude % 10UL));
        magnitude /= 10UL;
    }
    while (index > 0) {
        cc64_putc(digits[--index]);
    }
}

/* Ask whether a character is waiting, up to a bound, and report how many
   attempts it took so the wait is visible rather than implied. */
static int wait_for_input(long attempts) {
    for (long i = 0L; i < attempts; ++i) {
        struct pollfd waiting;
        waiting.fd = 0;
        waiting.events = 1; /* POLLIN */
        waiting.revents = 0;
        if (poll(&waiting, 1, 0) > 0) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    if (!wait_for_input(40000000L)) {
        say("console input: nothing was ever reported ready\n");
        return 1;
    }
    unsigned char got = 0;
    if (read(0, &got, 1) != 1) {
        say("console input: reported ready, then read returned nothing\n");
        return 1;
    }
    if (got != 'Z') {
        say("console input: read a character, but not the one sent: ");
        cc64_write(1, &got, 1);
        say("\n");
        return 1;
    }
    say("console input: poll reported ready and the read returned the character\n");
    return 0;
}
