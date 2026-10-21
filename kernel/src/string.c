#include "string.h"

size_t strlen(const char *s)
{
    size_t len = 0;

    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void *memcpy(void *dst, const void *src, size_t size)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    for (size_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int value, size_t size)
{
    unsigned char *d = dst;

    for (size_t i = 0; i < size; i++) {
        d[i] = (unsigned char)value;
    }
    return dst;
}
