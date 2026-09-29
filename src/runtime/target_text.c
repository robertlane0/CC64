/* Target string and error-text routines that the target's own library does not
 * otherwise provide.
 *
 * The search and tokenizing routines here are the ones a program reaches for
 * when it is reading structured text: a byte inside a counted run, a byte
 * inside a counted run counted from the end, and a run of text split on a set
 * of separators with a place to keep the position between calls. Each is
 * written so that a null result means the same thing it means elsewhere, and a
 * caller that passes a count of zero is answered without reading any byte,
 * because a run of no bytes contains nothing to find. */

#include <ctype.h>
#include <stddef.h>
#include <string.h>

void *memchr(const void *bytes, int value, size_t size)
{
    const unsigned char *at = (const unsigned char *)bytes;
    unsigned char wanted = (unsigned char)value;
    for (size_t index = 0U; index < size; ++index) {
        if (at[index] == wanted) return (void *)(at + index);
    }
    return NULL;
}

void *memrchr(const void *bytes, int value, size_t size)
{
    const unsigned char *at = (const unsigned char *)bytes;
    unsigned char wanted = (unsigned char)value;
    while (size > 0U) {
        --size;
        if (at[size] == wanted) return (void *)(at + size);
    }
    return NULL;
}

char *strtok_r(char *text, const char *separators, char **state)
{
    if (state == NULL || separators == NULL) return NULL;
    char *cursor = text != NULL ? text : *state;
    if (cursor == NULL) return NULL;
    /* A separator is a set rather than a string, so a character is a separator
       if the set mentions it anywhere. */
    while (*cursor != '\0' && strchr(separators, *cursor) != NULL) ++cursor;
    if (*cursor == '\0') {
        *state = NULL;
        return NULL;
    }
    char *token = cursor;
    while (*cursor != '\0' && strchr(separators, *cursor) == NULL) ++cursor;
    if (*cursor != '\0') {
        *cursor = '\0';
        *state = cursor + 1;
    } else {
        *state = NULL;
    }
    return token;
}

/* The position a tokenizing call without a caller-supplied place keeps. It is
   per-call rather than global because the target runs one program at a time
   and the state belongs to the caller's own recursion, which this cannot know
   about; a program that nests two tokenizations uses the form that takes a
   place to keep it. */
static char *token_state;

char *strtok(char *text, const char *separators)
{
    return strtok_r(text, separators, &token_state);
}

int strcasecmp(const char *left, const char *right)
{
    for (;;) {
        int a = tolower((unsigned char)*left);
        int b = tolower((unsigned char)*right);
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;
        ++left;
        ++right;
    }
}

int strncasecmp(const char *left, const char *right, size_t size)
{
    for (size_t index = 0U; index < size; ++index) {
        int a = tolower((unsigned char)left[index]);
        int b = tolower((unsigned char)right[index]);
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;
    }
    return 0;
}

/* The target reports a failure as a number, and a program that shows the
   number to a person needs words for it. The table is the header's own list,
   in the header's own order, so the text a program gets for a number is the
   text its compile-time constants name. A number outside the list is described
   rather than indexed, because indexing past the end would print whatever
   happened to follow. */
struct cc64_error_text {
    int code;
    const char *text;
};

static const struct cc64_error_text error_texts[] = {
    {1, "Operation not permitted"},
    {2, "No such file or directory"},
    {3, "No such process"},
    {4, "Interrupted system call"},
    {5, "Input/output error"},
    {6, "No such device or address"},
    {7, "Argument list too long"},
    {8, "Exec format error"},
    {9, "Bad file descriptor"},
    {10, "No child processes"},
    {11, "Resource temporarily unavailable"},
    {12, "Cannot allocate memory"},
    {13, "Permission denied"},
    {14, "Bad address"},
    {16, "Device or resource busy"},
    {17, "File exists"},
    {18, "Invalid cross-device link"},
    {19, "No such device"},
    {20, "Not a directory"},
    {21, "Is a directory"},
    {22, "Invalid argument"},
    {23, "Too many open files"},
    {24, "Too many open files in system"},
    {25, "Inappropriate ioctl for device"},
    {27, "File too large"},
    {28, "No space left on device"},
    {29, "Illegal seek"},
    {30, "Read-only file system"},
    {31, "Too many links"},
    {32, "Broken pipe"},
    {36, "File name too long"},
    {38, "Function not implemented"},
    {40, "Too many levels of symbolic links"}
};

/* A description of a number the table does not hold. It goes in one fixed
   record rather than on the heap because a program that reports an error while
   its own memory is exhausted must still get a pointer back. One target runs
   one program, so the record belongs to the last caller rather than being
   shared, which is what a single-threaded interface can promise. */
static char unknown_text[32];

static char *describe(int code, size_t size)
{
    char *buffer = unknown_text;
    const char prefix[] = "Unknown error ";
    size_t at = 0U;
    while (at + 1U < size && at < sizeof(prefix) - 1U) {
        buffer[at] = prefix[at];
        ++at;
    }
    unsigned long value = code < 0 ? 0UL : (unsigned long)code;
    char digits[24];
    size_t count = 0U;
    do {
        digits[count] = (char)('0' + (int)(value % 10UL));
        ++count;
        value /= 10UL;
    } while (value != 0UL && count < sizeof(digits));
    while (count > 0U && at + 1U < size) {
        --count;
        buffer[at] = digits[count];
        ++at;
    }
    buffer[at < size ? at : size - 1U] = '\0';
    return buffer;
}

char *strerror(int code)
{
    for (size_t index = 0U;
         index < sizeof(error_texts) / sizeof(error_texts[0]); ++index) {
        if (error_texts[index].code == code) return (char *)error_texts[index].text;
    }
    return describe(code, sizeof(unknown_text));
}
