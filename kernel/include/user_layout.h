#ifndef USER_LAYOUT_H
#define USER_LAYOUT_H

/* One raw-image window shared by all user tasks; no page tables yet. */
#ifdef QEMU
#define USER_CODE_BASE 0x83000000UL
#else
#define USER_CODE_BASE 0x32000000UL
#endif
#define USER_CODE_SIZE  0x00100000UL
#define USER_CODE_LIMIT (USER_CODE_BASE + USER_CODE_SIZE)
#define USER_STACK_SIZE 0x00010000UL

#endif
