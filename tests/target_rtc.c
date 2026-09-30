/* Report the target's date, read through the documented service boundary.
 *
 * The target's own suite covers the set/get round trip, but a test that lives
 * in the compiler is what makes the defect a *reproduced* one: the harness
 * sets the date with the target's own command, this program reads it back, and
 * the pair fails against the target before the fix and passes after it.
 *
 * A clock asked to move its month while it still holds a day the new month
 * does not have has to resolve a date that never existed, and what it resolves
 * to is not the date it was given. The date the harness sets crosses a month
 * boundary, which is what makes that reachable. The program does not choose
 * the date: it reports what the target holds, so the reading is the target's
 * and not this program's.
 *
 * Exit 0 when the date is a usable one, 1 when it is not, so a target that
 * hands back a nonsensical date fails by name rather than by printing.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int cc64_write(int handle, const void *data, unsigned long size);
int cc64_putc(int character);

/* INT 21h AH=2Ah returns CX=year, DH=month, DL=day. The library folds the
   fields into one value, and the year is the low half. */
long cc64_date_fields(void);

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

static void say(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    cc64_write(1, text, (unsigned long)length);
}

int main(void) {
    long fields = cc64_date_fields();
    long year = fields & 0xFFFFL;
    long month_day = (fields >> 16) & 0xFFFFL;
    long month = (month_day >> 8) & 0xFFL;
    long day = month_day & 0xFFL;

    put(year);
    say("-");
    put(month);
    say("-");
    put(day);
    say("\n");

    if (year < 1980L || year > 2099L) {
        say("date: year out of range\n");
        return 1;
    }
    if (month < 1L || month > 12L) {
        say("date: month out of range\n");
        return 1;
    }
    if (day < 1L || day > 31L) {
        say("date: day out of range\n");
        return 1;
    }
    return 0;
}
