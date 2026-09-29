#ifndef CC64_TARGET_STRING_H
#define CC64_TARGET_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
size_t strlen(const char *text);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t size);
char *strcat(char *destination, const char *source);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t size);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
char *strpbrk(const char *text, const char *accept);
char *strstr(const char *text, const char *needle);
char *strdup(const char *text);
/* The byte search finds the first byte equal to the value, and the byte search
   backwards finds the last. Both return a null pointer when there is none,
   which is the same answer the character searches give. */
void *memchr(const void *bytes, int value, size_t size);
void *memrchr(const void *bytes, int value, size_t size);
/* The tokenizer keeps its position in the caller's own variable, so two
   tokenizations of two strings can be in progress at once. The state starts as
   a null pointer and each call replaces it. */
char *strtok_r(char *text, const char *separators, char **state);
char *strtok(char *text, const char *separators);
/* A description of a failure number. The text is the target's own, is
   the same for the same number every time, and is never null. */
char *strerror(int code);
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t size);

#endif
