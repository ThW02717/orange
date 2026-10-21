/*
 * Raw user images: exec_load_image() copies one image into the fixed window.
 * exec_spawn() creates a batch with private stacks, rolling it back on failure.
 * Shell and syscall code decide when to run or replace the current task.
 */
#include "exec.h"
#include "platform.h"
#include "cpio.h"
#include "memory.h"
#include "string.h"
#include "thread.h"
#include "thread_internal.h"
#include "sched.h"
#include "user_layout.h"

#define CPIO_MODE_TYPE_MASK 0170000U
#define CPIO_MODE_REGULAR 0100000U

/* Build the initramfs path used by runu: bin/<name>.bin. */
static int build_user_program_path(const char *name, char *out, unsigned int out_size)
{
    static const char prefix[] = "bin/";
    static const char suffix[] = ".bin";
    unsigned int pos = 0;
    unsigned int i = 0;

    if (name == 0 || out == 0 || out_size == 0) {
        return -1;
    }

    while (prefix[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = prefix[i++];
    }

    i = 0;
    while (name[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = name[i++];
    }

    i = 0;
    while (suffix[i] != '\0') {
        if (pos + 1 >= out_size) {
            return -1;
        }
        out[pos++] = suffix[i++];
    }

    out[pos] = '\0';
    return 0;
}

int exec_load_image(const char *name, uintptr_t *entry_out, unsigned long *size_out)
{
    const struct boot_info *boot = platform_boot_info();
    const void *data;
    unsigned long size;
    unsigned int mode;
    char path[64];

    if (name == 0 || entry_out == 0 || size_out == 0) {
        return -1;
    }
    if (boot->initrd_start == 0 || boot->initrd_end <= boot->initrd_start) {
        return -1;
    }
    if (build_user_program_path(name, path, sizeof(path)) != 0) {
        return -1;
    }
    if (cpio_find((const void *)boot->initrd_start, (const void *)boot->initrd_end,
                  path, &data, &size, &mode) != 0) {
        return -1;
    }
    if ((mode & CPIO_MODE_TYPE_MASK) != CPIO_MODE_REGULAR) {
        return -1;
    }
    if (size == 0 || size > USER_CODE_SIZE) {
        return -1;
    }
    if (thread_user_image_busy(thread_current())) {
        return -1;
    }

    memcpy((void *)(uintptr_t)USER_CODE_BASE, data, size);
    asm volatile(".word 0x0000100f" ::: "memory");
    *entry_out = (uintptr_t)USER_CODE_BASE;
    *size_out = size;
    return 0;
}

int exec_spawn(const char *name, unsigned int count, struct exec_result *result)
{
    unsigned int i;

    if (result == 0 || count == 0 || count > EXEC_MAX_INSTANCES ||
        exec_load_image(name, &result->entry, &result->size) != 0) {
        return -1;
    }
    thread_system_bootstrap_init();
    for (i = 0; i < count; i++) {
        uintptr_t stack = (uintptr_t)kmalloc(USER_STACK_SIZE);

        if (stack == 0) {
            goto fail;
        }
        result->tids[i] = user_process_create(result->entry, stack, stack + USER_STACK_SIZE);
        if (result->tids[i] < 0) {
            kfree((void *)stack);
            goto fail;
        }
    }
    return 0;

fail:
    /* No worker may outlive a failed batch launch. None has run yet. */
    while (i > 0) {
        thread_stop_tid(result->tids[--i], -1);
    }
    thread_reap_zombies();
    return -1;
}
