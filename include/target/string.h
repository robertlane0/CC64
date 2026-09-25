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

#endif
