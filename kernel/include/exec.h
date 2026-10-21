#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

#define EXEC_MAX_INSTANCES 2U

struct exec_result {
    uintptr_t entry;
    unsigned long size;
    int tids[EXEC_MAX_INSTANCES];
};

int exec_load_image(const char *name, uintptr_t *entry, unsigned long *size);
int exec_spawn(const char *name, unsigned int count, struct exec_result *result);

#endif
