#include <stdint.h>
#include "cpio.h"

#define CPIO_NEWC_MAGIC "070701"
// new ASCII format
struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};
// static: only this c file can use
static int hex_to_u32(const char *s, unsigned int *out) {
    unsigned int v = 0;
    unsigned int i;
    for (i = 0; i < 8; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned int)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned int)(c - 'A' + 10);
        } else {
            return -1;
        }
    }
    *out = v;
    return 0;
}

static uintptr_t align4(uintptr_t n) {
    return (n + 3U) & ~(uintptr_t)3U;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int cpio_iterate(const void *start, const void *end, cpio_iter_fn fn, void *ctx) {
    const uint8_t *cur = (const uint8_t *)start; // at start
    const uint8_t *limit = (const uint8_t *)end;

    if (start == 0 || end == 0 || fn == 0 || (uintptr_t)end < (uintptr_t)start) {
        return -1;
    }

    while (1) {
        const struct cpio_newc_header *hdr = (const struct cpio_newc_header *)cur;
        unsigned int namesz;
        unsigned int filesz;
        unsigned int mode;
        const char *name;
        const uint8_t *data;
        uintptr_t name_span;
        uintptr_t file_span;
        unsigned int i;

        if ((uintptr_t)cur > (uintptr_t)limit ||
            (uintptr_t)limit - (uintptr_t)cur < sizeof(*hdr)) {
            return -1;
        }

        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            return -1;
        }

        if (hex_to_u32(hdr->c_namesize, &namesz) != 0 ||
            hex_to_u32(hdr->c_filesize, &filesz) != 0 ||
            hex_to_u32(hdr->c_mode, &mode) != 0) {
            return -1;
        }
        // after header is name
        name = (const char *)(cur + sizeof(*hdr));
        if (namesz == 0 || (uintptr_t)namesz > (uintptr_t)limit - (uintptr_t)name) {
            return -1;
        }

        if (name[namesz - 1U] != '\0') {
            return -1;
        }
        for (i = 0; i + 1U < namesz; i++) {
            if (name[i] == '\0') {
                return -1;
            }
        }

        if (str_eq(name, "TRAILER!!!")) {
            return 0;
        }
        // The byte we skip
        name_span = align4(sizeof(*hdr) + (uintptr_t)namesz);
        if (name_span > (uintptr_t)limit - (uintptr_t)cur) {
            return -1;
        }

        // data
        data = cur + name_span;
        file_span = align4((uintptr_t)filesz);
        if (file_span > (uintptr_t)limit - (uintptr_t)data) {
            return -1;
        }

        fn(name, data, filesz, mode, ctx);

        cur = data + file_span;
    }
}

struct cpio_find_ctx {
    const char *name;
    const void **data;
    unsigned long *size;
    unsigned int *mode;
    int found;
};

static void cpio_find_cb(const char *name, const void *data, unsigned long size,
                         unsigned int mode, void *ctx) {
    struct cpio_find_ctx *st = (struct cpio_find_ctx *)ctx;
    if (st->found) {
        return;
    }
    if (str_eq(name, st->name)) {
        if (st->data) {
            *st->data = data;
        }
        if (st->size) {
            *st->size = size;
        }
        if (st->mode) {
            *st->mode = mode;
        }
        st->found = 1;
    }
}

int cpio_find(const void *start, const void *end, const char *name,
              const void **data, unsigned long *size, unsigned int *mode) {
    struct cpio_find_ctx ctx;
    if (name == 0) { return -1; }
    ctx.name = name;
    ctx.data = data;
    ctx.size = size;
    ctx.mode = mode;
    ctx.found = 0;

    if (cpio_iterate(start, end, cpio_find_cb, &ctx) != 0) {
        return -1;
    }
    return ctx.found ? 0 : -1;
}
