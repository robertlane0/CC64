/* Target formatted output for MS-DOS64.
 *
 * The compiler's diagnostics and token dumps need a small, exact subset of the
 * C conversion syntax: %s, %c, %d/%i, %u, %x/%X, %o, %p, %% with the z, l and
 * ll length modifiers and the 0 and - flags plus a decimal field width. No
 * floating conversion is provided, because no target call site formats one; a
 * conversion outside the subset is written as a visible marker rather than
 * being silently dropped, so an unsupported format can never be mistaken for
 * correct output.
 *
 * One sink serves both destinations: a bounded character buffer for snprintf
 * and a stream for the printf family. The variadic arguments are integer or
 * pointer values, which is the whole variadic contract recorded in the ABI
 * document. */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct cc64_sink {
    char *buffer;
    size_t capacity;
    size_t used;
    FILE *stream;
    int failed;
};

static void sink_put(struct cc64_sink *sink, int character)
{
    char byte = (char)character;
    if (sink->stream != NULL) {
        if (fwrite(&byte, 1U, 1U, sink->stream) != 1U) sink->failed = 1;
        return;
    }
    if (sink->buffer == NULL) return;
    if (sink->used + 1U < sink->capacity) {
        sink->buffer[sink->used] = byte;
        ++sink->used;
    } else {
        sink->failed = 1;
    }
}

static void sink_text(struct cc64_sink *sink, const char *text, size_t length)
{
    size_t index = 0U;
    while (index < length) {
        sink_put(sink, (int)(unsigned char)text[index]);
        ++index;
    }
}

static void sink_padding(struct cc64_sink *sink, int count, char fill)
{
    while (count > 0) {
        sink_put(sink, (int)(unsigned char)fill);
        --count;
    }
}

static void sink_unsigned(struct cc64_sink *sink, unsigned long value,
                           unsigned base, int upper, int width, int zero,
                           int negative)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = lower_digits;
    if (upper) digits = upper_digits;
    char digits_buffer[72];
    size_t length = 0U;
    if (value == 0UL) {
        digits_buffer[length] = '0';
        ++length;
    }
    while (value != 0UL) {
        digits_buffer[length] = digits[value % base];
        ++length;
        value = value / base;
    }
    int sign = negative ? 1 : 0;
    int padding = width - (int)length - sign;
    if (!zero) sink_padding(sink, padding, ' ');
    if (sign != 0) sink_put(sink, '-');
    if (zero) sink_padding(sink, padding, '0');
    while (length > 0U) {
        --length;
        sink_put(sink, (int)(unsigned char)digits_buffer[length]);
    }
}

static void format_into(struct cc64_sink *sink, const char *format, va_list arguments)
{
    size_t index = 0U;
    while (format[index] != '\0') {
        if (format[index] != '%') {
            sink_put(sink, (int)(unsigned char)format[index]);
            ++index;
            continue;
        }
        ++index;
        if (format[index] == '%') {
            sink_put(sink, '%');
            ++index;
            continue;
        }
        int zero = 0;
        int left = 0;
        int width = 0;
        for (;;) {
            if (format[index] == '0') { zero = 1; ++index; continue; }
            if (format[index] == '-') { left = 1; ++index; continue; }
            if (format[index] >= '0' && format[index] <= '9') {
                width = width * 10 + (int)(format[index] - '0');
                ++index;
                continue;
            }
            break;
        }
        int size = 0;
        if (format[index] == 'z') {
            size = 1;
            ++index;
        } else if (format[index] == 'l') {
            size = 2;
            ++index;
            if (format[index] == 'l') { size = 3; ++index; }
        }
        int conversion = (int)(unsigned char)format[index];
        if (conversion == '\0') break;
        ++index;
        if (conversion == 's') {
            const char *text = va_arg(arguments, const char *);
            const char *shown = text == NULL ? "(null)" : text;
            size_t length = text == NULL ? 6U : strlen(text);
            int padding = width - (int)length;
            if (left) {
                sink_text(sink, shown, length);
                sink_padding(sink, padding, ' ');
            } else {
                sink_padding(sink, padding, ' ');
                sink_text(sink, shown, length);
            }
            continue;
        }
        if (conversion == 'c') {
            int character = va_arg(arguments, int);
            int padding = width - 1;
            if (left) {
                sink_put(sink, character);
                sink_padding(sink, padding, ' ');
            } else {
                sink_padding(sink, padding, ' ');
                sink_put(sink, character);
            }
            continue;
        }
        if (conversion == 'd' || conversion == 'i') {
            long value = size >= 3 ? va_arg(arguments, long long)
                                   : (long)va_arg(arguments, int);
            unsigned long magnitude = value < 0L
                                          ? (unsigned long)0 - (unsigned long)value
                                          : (unsigned long)value;
            sink_unsigned(sink, magnitude, 10UL, 0, width, zero, value < 0L);
            continue;
        }
        if (conversion == 'u' || conversion == 'x' || conversion == 'X' ||
            conversion == 'o' || conversion == 'p') {
            unsigned long value = (unsigned long)va_arg(arguments, size_t);
            unsigned long base = 10UL;
            int upper = 0;
            if (conversion == 'x') base = 16UL;
            else if (conversion == 'X') { base = 16UL; upper = 1; }
            else if (conversion == 'o') base = 8UL;
            else if (conversion == 'p') {
                base = 16UL;
                sink_text(sink, "0x", 2U);
                width -= 2;
            }
            sink_unsigned(sink, value, base, upper, width, zero, 0);
            continue;
        }
        /* An unsupported conversion is shown, never silently dropped. */
        sink_text(sink, "%!", 2U);
        sink_put(sink, conversion);
    }
}

int vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments)
{
    struct cc64_sink sink;
    sink.buffer = buffer;
    sink.capacity = capacity;
    sink.used = 0U;
    sink.stream = NULL;
    sink.failed = 0;
    format_into(&sink, format, arguments);
    if (buffer != NULL && capacity != 0U) {
        buffer[sink.used < capacity ? sink.used : capacity - 1U] = '\0';
    }
    return (int)sink.used;
}

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
    struct cc64_sink sink;
    sink.buffer = NULL;
    sink.capacity = 0U;
    sink.used = 0U;
    sink.stream = stream;
    sink.failed = 0;
    format_into(&sink, format, arguments);
    return (int)sink.used;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return written;
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int written = vfprintf(stream, format, arguments);
    va_end(arguments);
    return written;
}

int printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int written = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return written;
}
