#include <stdint.h>
#include "fdt.h"

#define FDT_MAGIC 0xd00dfeedU

#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_NOP        4U
#define FDT_END        9U

#define FDT_MAX_DEPTH 16

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct fdt_path_comp {
    const char *ptr;
    unsigned int len;
};

static inline uint32_t fdt32_to_cpu(uint32_t v) {
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8)  |
           ((v & 0x00ff0000U) >> 8)  |
           ((v & 0xff000000U) >> 24);
}

static unsigned int str_len(const char *s) {
    unsigned int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int name_eq(const char *name, const char *cmp, unsigned int len) {
    unsigned int i;
    for (i = 0; i < len; i++) {
        if (name[i] != cmp[i]) {
            return 0;
        }
    }
    return name[len] == '\0';
}

static unsigned int align4(unsigned int n) {
    return (n + 3U) & ~3U;
}

static int fdt_check_header(const void *fdt) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (fdt32_to_cpu(hdr->magic) != FDT_MAGIC) {
        return -1;
    }
    return 0;
}

int fdt_path_offset(const void *fdt, const char *path) {
    struct fdt_path_comp comps[FDT_MAX_DEPTH];
    unsigned int comp_count = 0;
    const char *p;
    const struct fdt_header *hdr;
    const uint8_t *struct_base;
    const uint8_t *cur;
    const uint8_t *end;
    int depth = -1;
    int match_depth = -1;

    if (fdt == 0 || path == 0) {
        return -1;
    }
    if (path[0] != '/') {
        return -1;
    }

    p = path + 1;
    while (*p != '\0') {
        const char *start = p;
        unsigned int len = 0;
        while (*p != '\0' && *p != '/') {
            p++;
            len++;
        }
        if (comp_count >= FDT_MAX_DEPTH) {
            return -1;
        }
        comps[comp_count].ptr = start;
        comps[comp_count].len = len;
        comp_count++;
        if (*p == '/') {
            p++;
        }
    }

    if (fdt_check_header(fdt) != 0) {
        return -1;
    }

    hdr = (const struct fdt_header *)fdt;
    struct_base = (const uint8_t *)fdt + fdt32_to_cpu(hdr->off_dt_struct);
    cur = struct_base;
    end = struct_base + fdt32_to_cpu(hdr->size_dt_struct);

    while (cur < end) {
        uint32_t token = fdt32_to_cpu(*(const uint32_t *)cur);
        cur += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)cur;
            unsigned int name_len = str_len(name);
            int node_offset = (int)((cur - 4) - struct_base);

            depth++;

            if (depth == 0) {
                match_depth = 0;
                if (comp_count == 0) {
                    return node_offset;
                }
            } else if (match_depth == depth - 1 && depth <= (int)comp_count) {
                if (name_eq(name, comps[depth - 1].ptr, comps[depth - 1].len)) {
                    match_depth = depth;
                    if (match_depth == (int)comp_count && depth == (int)comp_count) {
                        return node_offset;
                    }
                }
            }

            cur += align4(name_len + 1U);
            break;
        }
        case FDT_END_NODE:
            if (match_depth == depth) {
                match_depth = depth - 1;
            }
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*(const uint32_t *)cur);
            cur += 4;
            cur += 4;
            cur += align4(len);
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -1;
        default:
            return -1;
        }
    }

    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp) {
    const struct fdt_header *hdr;
    const uint8_t *struct_base;
    const uint8_t *strings_base;
    const uint8_t *cur;
    const uint8_t *end;
    int depth = 0;

    if (fdt == 0 || name == 0) {
        return 0;
    }
    if (fdt_check_header(fdt) != 0) {
        return 0;
    }

    hdr = (const struct fdt_header *)fdt;
    struct_base = (const uint8_t *)fdt + fdt32_to_cpu(hdr->off_dt_struct);
    strings_base = (const uint8_t *)fdt + fdt32_to_cpu(hdr->off_dt_strings);
    end = struct_base + fdt32_to_cpu(hdr->size_dt_struct);

    cur = struct_base + nodeoffset;
    if (cur + 4 > end) {
        return 0;
    }

    if (fdt32_to_cpu(*(const uint32_t *)cur) != FDT_BEGIN_NODE) {
        return 0;
    }
    cur += 4;

    {
        const char *node_name = (const char *)cur;
        cur += align4(str_len(node_name) + 1U);
    }

    while (cur < end) {
        uint32_t token = fdt32_to_cpu(*(const uint32_t *)cur);
        cur += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *n = (const char *)cur;
            cur += align4(str_len(n) + 1U);
            depth++;
            break;
        }
        case FDT_END_NODE:
            if (depth == 0) {
                return 0;
            }
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*(const uint32_t *)cur);
            uint32_t nameoff;
            const char *prop_name;
            const void *prop_data;

            cur += 4;
            nameoff = fdt32_to_cpu(*(const uint32_t *)cur);
            cur += 4;
            prop_name = (const char *)(strings_base + nameoff);
            prop_data = (const void *)cur;
            cur += align4(len);

            if (name_eq(prop_name, name, str_len(name))) {
                if (lenp != 0) {
                    *lenp = (int)len;
                }
                return prop_data;
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return 0;
        default:
            return 0;
        }
    }

    return 0;
}

static int fdt_get_u32(const void *fdt, int node, const char *name, uint32_t defval) {
    int len = 0;
    const uint32_t *prop = (const uint32_t *)fdt_getprop(fdt, node, name, &len);
    if (prop == 0 || len < 4) {
        return (int)defval;
    }
    return (int)fdt32_to_cpu(prop[0]);
}

static int fdt_get_u64_prop(const void *fdt, int node, const char *name, uint64_t *out) {
    int len = 0;
    const uint32_t *prop = (const uint32_t *)fdt_getprop(fdt, node, name, &len);
    if (prop == 0 || out == 0) {
        return -1;
    }
    if (len >= 8) {
        uint64_t hi = (uint64_t)fdt32_to_cpu(prop[0]);
        uint64_t lo = (uint64_t)fdt32_to_cpu(prop[1]);
        *out = (hi << 32) | lo;
        return 0;
    }
    if (len >= 4) {
        *out = (uint64_t)fdt32_to_cpu(prop[0]);
        return 0;
    }
    return -1;
}

int fdt_get_memory_region(const void *fdt, uint64_t *base, uint64_t *size) {
    int root;
    int node;
    int len = 0;
    int addr_cells;
    int size_cells;
    const uint32_t *reg;
    uint64_t addr = 0;
    uint64_t sz = 0;

    if (base == 0 || size == 0) {
        return -1;
    }

    if (fdt_check_header(fdt) != 0) {
        return -1;
    }

    root = fdt_path_offset(fdt, "/");
    if (root < 0) {
        return -1;
    }

    addr_cells = fdt_get_u32(fdt, root, "#address-cells", 2);
    size_cells = fdt_get_u32(fdt, root, "#size-cells", 2);

    node = fdt_path_offset(fdt, "/memory@0");
    if (node < 0) {
        node = fdt_path_offset(fdt, "/memory@80000000");
    }
    if (node < 0) {
        return -1;
    }

    reg = (const uint32_t *)fdt_getprop(fdt, node, "reg", &len);
    if (reg == 0) {
        return -1;
    }

    if (len < (addr_cells + size_cells) * 4) {
        return -1;
    }

    if (addr_cells == 1) {
        addr = fdt32_to_cpu(reg[0]);
        reg += 1;
    } else if (addr_cells == 2) {
        addr = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
        reg += 2;
    } else {
        return -1;
    }

    if (size_cells == 1) {
        sz = fdt32_to_cpu(reg[0]);
    } else if (size_cells == 2) {
        sz = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
    } else {
        return -1;
    }

    *base = addr;
    *size = sz;
    return 0;
}

int fdt_get_initrd_range(const void *fdt, uint64_t *start, uint64_t *end) {
    int node;
    uint64_t s = 0;
    uint64_t e = 0;

    if (start == 0 || end == 0) {
        return -1;
    }
    *start = 0;
    *end = 0;

    if (fdt_check_header(fdt) != 0) {
        return -1;
    }

    node = fdt_path_offset(fdt, "/chosen");
    if (node < 0) {
        return -1;
    }

    if (fdt_get_u64_prop(fdt, node, "linux,initrd-start", &s) != 0) {
        return -1;
    }
    if (fdt_get_u64_prop(fdt, node, "linux,initrd-end", &e) != 0) {
        return -1;
    }
    if (e <= s) {
        return -1;
    }

    *start = s;
    *end = e;
    return 0;
}
