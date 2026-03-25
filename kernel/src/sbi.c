#include "sbi.h"
#include <stdint.h>

/* Common SBI call wrapper. All board-specific firmware services flow through
 * this register convention before being exposed as typed helpers.
 */
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

/* BASE extension helpers are used to discover which SBI services the current
 * firmware actually implements.
 */
static long __sbi_base_ecall(int fid) {
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, fid, 0, 0, 0, 0, 0, 0);
    if (ret.error) {
        return 0;
    }
    return ret.value;
}

long sbi_get_spec_version() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_SPEC_VERSION);
}

long sbi_get_impl_id() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_IMPL_ID);
}

long sbi_get_impl_version() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_IMPL_VERSION);
}

long sbi_get_mimpid() {
    return __sbi_base_ecall(SBI_EXT_BASE_GET_MIMPID);
}

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

long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
                                  hartid, start_addr, opaque, 0, 0, 0);
    return ret.error;
}

long sbi_hart_get_status(unsigned long hartid) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_GET_STATUS,
                                  hartid, 0, 0, 0, 0, 0);
    if (ret.error != SBI_SUCCESS) {
        return ret.error;
    }
    return ret.value;
}

struct sbiret sbi_set_timer(uint64_t stime_value)
{
    /* Prefer the SBI v0.2+ TIME extension when firmware advertises it. Some
     * vendor firmwares still only expose the legacy timer extension, so keep
     * a compatibility fallback for real board bring-up.
     */
    if (sbi_probe_extension(SBI_EXT_TIMER)) {
        return sbi_ecall(SBI_EXT_TIMER, 0,
                         (unsigned long)stime_value, 0, 0, 0, 0, 0);
    }

    return sbi_ecall(SBI_EXT_TIMER_LEGACY, 0,
                     (unsigned long)stime_value, 0, 0, 0, 0, 0);
}
