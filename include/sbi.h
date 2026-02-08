#ifndef _SBI_H
#define _SBI_H

#include <stdint.h>

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                        unsigned long arg1, unsigned long arg2,
                        unsigned long arg3, unsigned long arg4,
                        unsigned long arg5);

long sbi_get_spec_version();
long sbi_get_impl_id();
long sbi_get_impl_version();
long sbi_get_mimpid();

long sbi_probe_extension(long extension_id);

void sbi_legacy_reboot();
void sbi_system_reboot();

#endif
