#ifndef _SBI_H
#define _SBI_H

#include <stdint.h>

/* SBI extension IDs */
#define SBI_EXT_TIMER_LEGACY   0x00
#define SBI_EXT_BASE           0x10
#define SBI_EXT_HSM            0x48534D
#define SBI_EXT_TIMER          0x54494D45

/* SBI BASE function IDs */
#define SBI_EXT_BASE_GET_SPEC_VERSION  0x0
#define SBI_EXT_BASE_GET_IMPL_ID       0x1
#define SBI_EXT_BASE_GET_IMPL_VERSION  0x2
#define SBI_EXT_BASE_PROBE_EXT         0x3
#define SBI_EXT_BASE_GET_MIMPID        0x6

/* SBI HSM function IDs */
#define SBI_EXT_HSM_HART_START         0x0
#define SBI_EXT_HSM_HART_GET_STATUS    0x2

/* SBI HSM hart states */
#define SBI_HSM_STATE_STARTED          0
#define SBI_HSM_STATE_STOPPED          1
#define SBI_HSM_STATE_START_PENDING    2
#define SBI_HSM_STATE_STOP_PENDING     3
#define SBI_HSM_STATE_SUSPENDED        4
#define SBI_HSM_STATE_SUSPEND_PENDING  5
#define SBI_HSM_STATE_RESUME_PENDING   6

/* SBI return error codes */
#define SBI_SUCCESS               0
#define SBI_ERR_FAILED           -1
#define SBI_ERR_NOT_SUPPORTED    -2
#define SBI_ERR_INVALID_PARAM    -3
#define SBI_ERR_DENIED           -4
#define SBI_ERR_INVALID_ADDRESS  -5
#define SBI_ERR_ALREADY_AVAILABLE -6

struct sbiret {
    long error;
    long value;
};

/* Common SBI transport used by the typed wrappers below. */
struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                        unsigned long arg1, unsigned long arg2,
                        unsigned long arg3, unsigned long arg4,
                        unsigned long arg5);

long sbi_get_spec_version();
long sbi_get_impl_id();
long sbi_get_impl_version();
long sbi_get_mimpid();

long sbi_probe_extension(long extension_id);
int sbi_err_map_errno(long err);
long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque);
long sbi_hart_get_status(unsigned long hartid);

/* Program the next machine timer event through SBI firmware. */
struct sbiret sbi_set_timer(uint64_t stime_value);

#endif
