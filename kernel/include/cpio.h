#ifndef CPIO_H
#define CPIO_H

#include <stdint.h>

typedef void (*cpio_iter_fn)(const char *name,
                             const void *data,
                             unsigned long size,
                             unsigned int mode,
                             void *ctx);

int cpio_iterate(const void *start, const void *end, cpio_iter_fn fn, void *ctx);
int cpio_find(const void *start, const void *end, const char *name,
              const void **data, unsigned long *size, unsigned int *mode);

#endif
