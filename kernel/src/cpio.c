#include <stdint.h>
#include "cpio.h"

#define CPIO_NEWC_MAGIC "070701"

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

static unsigned int hex_to_u32(const char *s) {
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
            return 0;
        }
    }
    return v;
}

static unsigned int align4(unsigned int n) {
    return (n + 3U) & ~3U;
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
    const uint8_t *cur = (const uint8_t *)start;
    const uint8_t *limit = end ? (const uint8_t *)end : 0;

    if (start == 0 || fn == 0) {
        return -1;
    }

    while (1) {
        const struct cpio_newc_header *hdr = (const struct cpio_newc_header *)cur;
        unsigned int namesz;
        unsigned int filesz;
        unsigned int mode;
        const char *name;
        const uint8_t *data;
        unsigned int name_pad;
        unsigned int file_pad;

        if (limit && (const uint8_t *)(cur + sizeof(*hdr)) > limit) {
            return -1;
        }

        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            return -1;
        }

        namesz = hex_to_u32(hdr->c_namesize);
        filesz = hex_to_u32(hdr->c_filesize);
        mode = hex_to_u32(hdr->c_mode);

        name = (const char *)(cur + sizeof(*hdr));
        if (limit && (const uint8_t *)(name + namesz) > limit) {
            return -1;
        }

        if (namesz == 0) {
            return -1;
        }

        if (str_eq(name, "TRAILER!!!")) {
            return 0;
        }

        name_pad = align4(sizeof(*hdr) + namesz) - (unsigned int)(sizeof(*hdr) + namesz);
        data = (const uint8_t *)(name + namesz + name_pad);
        if (limit && data > limit) {
            return -1;
        }

        if (limit && data + filesz > limit) {
            return -1;
        }

        fn(name, data, filesz, mode, ctx);

        file_pad = align4(filesz) - filesz;
        cur = data + filesz + file_pad;
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
