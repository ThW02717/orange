#include "sbi.h"
#include <stdint.h>
// RISCV SBI RULE: 
// The universal SBI call wrapper
// Loads arguments into RISC-V calling-convention registers:
// a0..a5 = arg0..arg5
// a6 = fid (function ID)
// a7 = ext (extension ID)
// a0 = error code
// a1 = return value
struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                        unsigned long arg1, unsigned long arg2,
                        unsigned long arg3, unsigned long arg4,
                        unsigned long arg5) {
    struct sbiret ret;
    register uintptr_t a0 asm ("a0") = (uintptr_t)arg0;
    register uintptr_t a1 asm ("a1") = (uintptr_t)arg1;
    register uintptr_t a2 asm ("a2") = (uintptr_t)arg2;
    register uintptr_t a3 asm ("a3") = (uintptr_t)arg3;
    register uintptr_t a4 asm ("a4") = (uintptr_t)arg4;
    register uintptr_t a5 asm ("a5") = (uintptr_t)arg5;
    register uintptr_t a6 asm ("a6") = (uintptr_t)fid;
    register uintptr_t a7 asm ("a7") = (uintptr_t)ext;

    asm volatile ("ecall"
                  : "+r"(a0), "+r"(a1)
                  : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                  : "memory");

    ret.error = (long)a0;
    ret.value = (long)a1;
    return ret;
}
// BASE extension wrappers
static long __sbi_base_ecall(int fid) {
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, fid, 0, 0, 0, 0, 0, 0);
    if (ret.error) {
        return 0;
    }
    return ret.value;
}
// SBI spec version supported by OpenSBI
// What SBI spec/ABI version does the firmware expose?
// Your value 0x01000000 likely corresponds to SBI v1.0 encoded in a packed format.
long sbi_get_spec_version() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_SPEC_VERSION);
}
// Which SBI firmware implementation is providing the services?
// Your impl id = 1 commonly represents OpenSBI on many platforms.
long sbi_get_impl_id() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_IMPL_ID);
}
// Version of that implementation (the firmware’s own versioning).
long sbi_get_impl_version() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_IMPL_VERSION);
}
// Machine implementation ID
long sbi_get_mimpid() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_MIMPID);
}
// whether a given SBI extension is supported
long sbi_probe_extension(long extension_id) {
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT,
                                  (unsigned long)extension_id, 0, 0, 0, 0, 0);
    if (ret.error) {
        return 0;
    }
    return ret.value;
}

int sbi_err_map_errno(long err) {
    switch (err) {
        case SBI_SUCCESS:
            return 0;
        case SBI_ERR_DENIED:
            return -1;
        case SBI_ERR_INVALID_PARAM:
            return -2;
        case SBI_ERR_INVALID_ADDRESS:
            return -3;
        case SBI_ERR_NOT_SUPPORTED:
        case SBI_ERR_FAILED:
        default:
            return -4;
    }
}
// Requests OpenSBI to start a target hart at start_addr with an optional argument opaque. Returns ret.error (0 means success).
long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
                                  hartid, start_addr, opaque, 0, 0, 0);
    return ret.error;
}
// Queries the hart state
// On success returns a state value (e.g., started/stopped/start_pending).
long sbi_hart_get_status(unsigned long hartid) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_GET_STATUS,
                                  hartid, 0, 0, 0, 0, 0);
    if (ret.error != SBI_SUCCESS) {
        return ret.error;
    }
    return ret.value;
}
