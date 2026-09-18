#ifndef RISCVSIM_CPU_STATE_H
#define RISCVSIM_CPU_STATE_H

#include <stdint.h>

/* CPU状态 */
typedef struct RISCVSIMCPUState
{
    uint64_t clock;
    uint32_t regs[32];
    uint32_t reservation_addr; /* 当前实现的保留范围为一个对齐的 4 字节字。 */
    int reservation_valid;
    int trapped;
    uint32_t trap_cause, trap_pc, trap_value;
    /* M1：CSR 文件、当前特权级与退休计数。 */
    uint32_t csr[4096];
    uint32_t priv;
    uint64_t instret;
} RISCVSIMCPUState;

#endif
