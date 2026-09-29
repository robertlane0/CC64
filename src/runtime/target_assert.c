/* Target assertion failure reporting for MS-DOS64.
 *
 * A failed assertion in a freestanding program has nowhere to return to and no
 * runtime to unwind, so the only honest outcome is to say which condition
 * failed and where, and then to end the process through the target's own exit
 * path. The report goes to the error console handle rather than to the standard
 * output, because a program whose assertion failed is not in a state where its
 * output can be trusted. */

#include <assert.h>
#include <stdio.h>

void cc64_exit(int code);

void cc64_assert_failed(const char *expression, const char *file, int line)
{
    fputs("assertion failed: ", stderr);
    fputs(expression != 0 ? expression : "(no text)", stderr);
    fputs(" at ", stderr);
    fputs(file != 0 ? file : "(no file)", stderr);
    fputc(':', stderr);
    fprintf(stderr, "%d\n", line);
    fflush(stderr);
    cc64_exit(70);
}
