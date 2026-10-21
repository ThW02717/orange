#ifndef CONTEXT_OFFSETS_H
#define CONTEXT_OFFSETS_H

/* Byte offsets shared by C and assembly; checked in context_layout.c. */
#define TC_RA        0
#define TC_SP        8
#define TC_GP        16
#define TC_TP        24
#define TC_T0        32
#define TC_T1        40
#define TC_T2        48
#define TC_S0        56
#define TC_S1        64
#define TC_A0        72
#define TC_A1        80
#define TC_A2        88
#define TC_A3        96
#define TC_A4        104
#define TC_A5        112
#define TC_A6        120
#define TC_A7        128
#define TC_S2        136
#define TC_S3        144
#define TC_S4        152
#define TC_S5        160
#define TC_S6        168
#define TC_S7        176
#define TC_S8        184
#define TC_S9        192
#define TC_S10       200
#define TC_S11       208
#define TC_T3        216
#define TC_T4        224
#define TC_T5        232
#define TC_T6        240
#define TC_SEPC      248
#define TC_SSTATUS   256
#define TC_SCAUSE    264
#define TC_STVAL     272
#define TRAP_CONTEXT_SIZE 280
#define TRAP_CONTEXT_ALLOC_SIZE 288

#define THREAD_RA   0
#define THREAD_SP   8
#define THREAD_S0   16
#define THREAD_S1   24
#define THREAD_S2   32
#define THREAD_S3   40
#define THREAD_S4   48
#define THREAD_S5   56
#define THREAD_S6   64
#define THREAD_S7   72
#define THREAD_S8   80
#define THREAD_S9   88
#define THREAD_S10  96
#define THREAD_S11  104
#define THREAD_CONTEXT_SIZE 112

#endif
