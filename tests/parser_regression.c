#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpio.h"
#include "fdt.h"

static void hex_field(unsigned char *p, uint32_t v)
{
    char text[9];
    snprintf(text, sizeof(text), "%08x", v);
    memcpy(p, text, 8);
}

static size_t entry(unsigned char *p, const char *name, const char *data)
{
    size_t namesz = strlen(name) + 1, filesz = strlen(data);
    size_t start = (110 + namesz + 3) & ~(size_t)3;
    size_t end = start + ((filesz + 3) & ~(size_t)3);
    memset(p, 0, end);
    memset(p, '0', 110);
    memcpy(p, "070701", 6);
    hex_field(p + 14, 0100644);
    hex_field(p + 54, filesz);
    hex_field(p + 94, namesz);
    memcpy(p + 110, name, namesz);
    memcpy(p + start, data, filesz);
    return end;
}

static void ignore_entry(const char *name, const void *data, unsigned long size,
                         unsigned int mode, void *ctx)
{
    (void)name; (void)data; (void)size; (void)mode; (void)ctx;
}

static void cpio_tests(void)
{
    unsigned char archive[512], bad[512];
    const void *data;
    unsigned long size;
    size_t used = entry(archive, "hello", "test");
    used += entry(archive + used, "TRAILER!!!", "");
    assert(cpio_find(archive, archive + used, "hello", &data, &size, 0) == 0);
    assert(size == 4 && memcmp(data, "test", 4) == 0);
    assert(cpio_find(archive, archive + used, 0, 0, 0, 0) == -1);
    memcpy(bad, archive, used); bad[115] = 'X';
    assert(cpio_iterate(bad, bad + used, ignore_entry, 0) == -1);
    memcpy(bad, archive, used); bad[54] = 'x';
    assert(cpio_iterate(bad, bad + used, ignore_entry, 0) == -1);
    memcpy(bad, archive, used); hex_field(bad + 54, UINT32_MAX);
    assert(cpio_iterate(bad, bad + used, ignore_entry, 0) == -1);
    memcpy(bad, archive, used); hex_field(bad + 94, UINT32_MAX);
    assert(cpio_iterate(bad, bad + used, ignore_entry, 0) == -1);
    assert(cpio_iterate(archive, archive + 109, ignore_entry, 0) == -1);
    assert(cpio_iterate(archive, archive + 119, ignore_entry, 0) == -1);
    assert(cpio_iterate(archive, 0, ignore_entry, 0) == -1);
    puts("[REGRESSION] CPIO bounds/hex/name/overflow: PASS");
}

static uint32_t be32(uint32_t v)
{
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

static void fdt_tests(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    long size;
    unsigned char *dtb, *bad;
    struct fdt_mem_region regions[16];
    int parent, child, len;
    uint32_t *header;
    assert(file);
    fseek(file, 0, SEEK_END); size = ftell(file); rewind(file);
    assert(size > 40);
    dtb = malloc(size); bad = malloc(size);
    assert(dtb && bad && fread(dtb, 1, size, file) == (size_t)size);
    fclose(file);
    assert(fdt_totalsize(dtb) == (uint32_t)size);
    parent = fdt_path_offset(dtb, "/parent"); child = fdt_path_offset(dtb, "/parent/child");
    assert(parent >= 0 && child > parent);
    assert(fdt_getprop(dtb, parent, "child-only", &len) == 0);
    assert(fdt_getprop(dtb, child, "child-only", &len) != 0);
    assert(fdt_getprop(dtb, -4, "compatible", &len) == 0);
    assert(fdt_getprop(dtb, 0x7fffffff, "compatible", &len) == 0);
    assert(fdt_find_compatible(dtb, "test,child") == child);
    assert(fdt_get_memory_regions(dtb, regions, 16) == 1 && regions[0].base == 0x80000000);
    assert(fdt_get_memreserve_regions(dtb, regions, 16) == 1 && regions[0].size == 0x1000);
    assert(fdt_get_reserved_memory_regions(dtb, regions, 16) == 1 && regions[0].base == 0x80010000);
    assert(fdt_get_initrd_range(0, &regions[0].base, &regions[0].size) == -1);
    memcpy(bad, dtb, size); header = (uint32_t *)bad; header[3] = be32(size + 4);
    assert(fdt_totalsize(bad) == 0);
    memcpy(bad, dtb, size); header[9] = be32(4);
    assert(fdt_path_offset(bad, "/") == -1);
    memcpy(bad, dtb, size);
    {
        const unsigned char *prop = fdt_getprop(bad, child, "compatible", &len);
        assert(prop && len > 0); bad[prop - bad + len - 1] = 'X';
        assert(fdt_find_compatible(bad, "test,child") == -1);
    }
    for (unsigned int i = 0; i < (unsigned int)size; i++) {
        memcpy(bad, dtb, size); bad[i] ^= 0xff;
        /* Keep the externally supplied buffer size truthful for this API. */
        if (i >= 4 && i < 8) { continue; }
        (void)fdt_totalsize(bad);
        (void)fdt_path_offset(bad, "/parent");
        (void)fdt_getprop(bad, parent, "compatible", &len);
        (void)fdt_find_compatible(bad, "test,child");
        (void)fdt_get_memory_regions(bad, regions, 16);
        (void)fdt_get_memreserve_regions(bad, regions, 16);
        (void)fdt_get_reserved_memory_regions(bad, regions, 16);
    }
    free(bad); free(dtb);
    puts("[REGRESSION] FDT scope/reservations/malformed-token mutations: PASS");
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    cpio_tests(); fdt_tests(argv[1]);
    if (argc == 3) {
        FILE *file = fopen(argv[2], "rb");
        long size;
        void *dtb;
        struct fdt_mem_region regions[16];
        uint64_t base;
        uint32_t irq;
        int uart, plic;
        assert(file);
        fseek(file, 0, SEEK_END); size = ftell(file); rewind(file);
        dtb = malloc(size);
        assert(dtb && fread(dtb, 1, size, file) == (size_t)size);
        fclose(file);
        assert(fdt_totalsize(dtb) == (uint32_t)size);
        assert(fdt_get_memory_regions(dtb, regions, 16) > 0);
        assert(fdt_get_memreserve_regions(dtb, regions, 16) >= 0);
        assert(fdt_get_reserved_memory_regions(dtb, regions, 16) >= 0);
        uart = fdt_find_compatible(dtb, "ky,pxa-uart");
        plic = fdt_find_compatible(dtb, "riscv,plic0");
        assert(uart >= 0 && plic >= 0);
        assert(fdt_get_reg_base(dtb, uart, &base) == 0 && base != 0);
        assert(fdt_get_interrupt_id(dtb, uart, &irq) == 0 && irq != 0);
        assert(fdt_get_reg_base(dtb, plic, &base) == 0 && base != 0);
        free(dtb);
        puts("[REGRESSION] supplied OrangePi RV2 DTB discovery: PASS");
    }
    return 0;
}
