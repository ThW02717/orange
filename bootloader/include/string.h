#ifndef STRING_H
#define STRING_H

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memcpy(void *dst, const void *src, size_t size);
void *memset(void *dst, int value, size_t size);

#endif
