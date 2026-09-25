/* Target string routines for MS-DOS64.
 *
 * The compiler's own sources use this file when CC64 runs on the target, so
 * every routine here is written in the documented C17 subset and depends on
 * nothing but the target service boundary. */

#include <stddef.h>
#include <string.h>

void *memcpy(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    size_t index = 0U;
    while (index < size) {
        out[index] = in[index];
        ++index;
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out == in || size == 0U) return destination;
    if (out < in) {
        size_t index = 0U;
        while (index < size) {
            out[index] = in[index];
            ++index;
        }
    } else {
        size_t index = size;
        while (index > 0U) {
            --index;
            out[index] = in[index];
        }
    }
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    unsigned char byte = (unsigned char)value;
    size_t index = 0U;
    while (index < size) {
        out[index] = byte;
        ++index;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    size_t index = 0U;
    while (index < size) {
        if (a[index] != b[index]) {
            return a[index] < b[index] ? -1 : 1;
        }
        ++index;
    }
    return 0;
}

size_t strlen(const char *text)
{
    size_t length = 0U;
    while (text[length] != '\0') ++length;
    return length;
}

char *strcpy(char *destination, const char *source)
{
    size_t index = 0U;
    while (source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return destination;
}

char *strncpy(char *destination, const char *source, size_t size)
{
    size_t index = 0U;
    while (index < size && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < size) {
        destination[index] = '\0';
        ++index;
    }
    return destination;
}

char *strcat(char *destination, const char *source)
{
    size_t at = strlen(destination);
    size_t index = 0U;
    while (source[index] != '\0') {
        destination[at] = source[index];
        ++at;
        ++index;
    }
    destination[at] = '\0';
    return destination;
}

int strcmp(const char *left, const char *right)
{
    size_t index = 0U;
    for (;;) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a != b) return a < b ? -1 : 1;
        if (a == '\0') return 0;
        ++index;
    }
}

int strncmp(const char *left, const char *right, size_t size)
{
    size_t index = 0U;
    while (index < size) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a != b) return a < b ? -1 : 1;
        if (a == '\0') return 0;
        ++index;
    }
    return 0;
}

char *strchr(const char *text, int character)
{
    unsigned char target = (unsigned char)character;
    size_t index = 0U;
    for (;;) {
        if ((unsigned char)text[index] == target) return (char *)text + index;
        if (text[index] == '\0') return NULL;
        ++index;
    }
}

char *strrchr(const char *text, int character)
{
    unsigned char target = (unsigned char)character;
    const char *found = NULL;
    size_t index = 0U;
    for (;;) {
        if ((unsigned char)text[index] == target) found = text + index;
        if (text[index] == '\0') break;
        ++index;
    }
    return (char *)found;
}

char *strpbrk(const char *text, const char *accept)
{
    size_t index = 0U;
    while (text[index] != '\0') {
        size_t other = 0U;
        while (accept[other] != '\0') {
            if (text[index] == accept[other]) return (char *)text + index;
            ++other;
        }
        ++index;
    }
    return NULL;
}

char *strstr(const char *text, const char *needle)
{
    if (needle[0] == '\0') return (char *)text;
    size_t index = 0U;
    while (text[index] != '\0') {
        size_t offset = 0U;
        while (needle[offset] != '\0' && text[index + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') return (char *)text + index;
        ++index;
    }
    return NULL;
}
