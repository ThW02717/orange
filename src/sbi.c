#include "sbi.h"
#include <stdint.h>



struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                        unsigned long arg1, unsigned long arg2,
                        unsigned long arg3, unsigned long arg4,
                        unsigned long arg5) {
    struct sbiret ret;

    __asm__ volatile (
        "mv a7, %[ext]\n" 
        "mv a6, %[fid]\n" 
        "mv a0, %[arg0]\n"
        "mv a1, %[arg1]\n" 
        "mv a2, %[arg2]\n"
        "mv a3, %[arg3]\n" 
        "mv a4, %[arg4]\n" 
        "mv a5, %[arg5]\n"
        
        "ecall\n"

        "mv %[err], a0\n"     
        "mv %[val], a1\n"   

        : [err] "=r" (ret.error), [val] "=r" (ret.value)
        
        : [ext] "r" (ext), [fid] "r" (fid),
          [arg0] "r" (arg0), [arg1] "r" (arg1), [arg2] "r" (arg2),
          [arg3] "r" (arg3), [arg4] "r" (arg4), [arg5] "r" (arg5)
        
        : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "memory"
    );

    return ret;
}


long sbi_get_spec_version() {

    struct sbiret ret = sbi_ecall(0x10, 0, 0, 0, 0, 0, 0, 0);
    return ret.value;
}

long sbi_get_impl_id() {
    struct sbiret ret = sbi_ecall(0x10, 1, 0, 0, 0, 0, 0, 0);
    return ret.value;
}

long sbi_get_impl_version() {
    struct sbiret ret = sbi_ecall(0x10, 2, 0, 0, 0, 0, 0, 0);
    return ret.value;
}

long sbi_get_mimpid() {
    struct sbiret ret = sbi_ecall(0x10, 6, 0, 0, 0, 0, 0, 0);
    return ret.value;
}


long sbi_probe_extension(long extension_id) {

    struct sbiret ret = sbi_ecall(0x10, 3, extension_id, 0, 0, 0, 0, 0);
    
    return ret.value;
}

void sbi_legacy_reboot() {
    sbi_ecall(0x08, 0, 0, 0, 0, 0, 0, 0);
}

void sbi_system_reboot() {
    /* SRST extension: reset_type=0(cold reboot), reset_reason=0(no reason). */
    struct sbiret ret = sbi_ecall(0x53525354, 0, 0, 0, 0, 0, 0, 0);
    if (ret.error != 0) {
        sbi_legacy_reboot();
    }
}
