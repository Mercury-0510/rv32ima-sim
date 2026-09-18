#ifndef RISCVSIM_CSR_H
#define RISCVSIM_CSR_H

#include <stdint.h>
#include "cpu_state.h"

/* 特权级编码，与 mstatus.MPP 字段一致。 */
#define PRIV_U 0u
#define PRIV_S 1u
#define PRIV_M 3u

/* CSR 地址（低 12 位）。M1 实现 M 模式集与计数器，S 模式寄存器先占位。 */
#define CSR_SSTATUS    0x100u
#define CSR_SIE        0x104u
#define CSR_STVEC      0x105u
#define CSR_SCOUNTEREN 0x106u
#define CSR_SSCRATCH   0x140u
#define CSR_SEPC       0x141u
#define CSR_SCAUSE     0x142u
#define CSR_STVAL      0x143u
#define CSR_SIP        0x144u
#define CSR_SATP       0x180u
#define CSR_MSTATUS    0x300u
#define CSR_MISA       0x301u
#define CSR_MEDELEG    0x302u
#define CSR_MIDELEG    0x303u
#define CSR_MIE        0x304u
#define CSR_MTVEC      0x305u
#define CSR_MCOUNTEREN 0x306u
#define CSR_MSCRATCH   0x340u
#define CSR_MEPC       0x341u
#define CSR_MCAUSE     0x342u
#define CSR_MTVAL      0x343u
#define CSR_MIP        0x344u
#define CSR_MVENDORID  0xf11u
#define CSR_MARCHID    0xf12u
#define CSR_MIMPID     0xf13u
#define CSR_MHARTID    0xf14u
#define CSR_CYCLE      0xc00u
#define CSR_TIME       0xc01u
#define CSR_INSTRET    0xc02u
#define CSR_CYCLEH     0xc80u
#define CSR_TIMEH      0xc81u
#define CSR_INSTRETH   0xc82u

/* mcounteren / scounteren 位。 */
#define COUNTER_CYCLE   1u
#define COUNTER_TIME    2u
#define COUNTER_INSTRET 4u

/* CSR 指令的 funct3 编码。 */
#define CSR_F3_RW  1u
#define CSR_F3_RS  2u
#define CSR_F3_RC  3u
#define CSR_F3_RWI 5u
#define CSR_F3_RSI 6u
#define CSR_F3_RCI 7u

/* misa：MXL=1（32 位）| 扩展 I | 扩展 M。A 尚未实现，故不置位。 */
#define MISA_VALUE ((UINT32_C(1) << 30) | (UINT32_C(1) << 8) | (UINT32_C(1) << 12))

/* 按地址位编码检查访问：addr[11:10] 读写权限（>=2 为只读），addr[9:8] 最低特权级。
 * 返回 0 表示允许，-1 表示非法。write 非 0 表示本次为写访问。 */
int csr_permitted(uint32_t addr, uint32_t priv, int write);

/* 读取 CSR；成功返回 0 并写 *value，非法访问返回 -1。 */
int csr_read(const struct RISCVSIMCPUState *cpu, uint32_t addr, uint32_t *value);

/* 内部写入（不含权限检查）；只读或计算类 CSR 的写入被忽略。 */
void csr_store(struct RISCVSIMCPUState *cpu, uint32_t addr, uint32_t value);

/* 执行一条 CSR 指令（读-改-写）。成功返回 0，*old 为读到的旧值；非法返回 -1。
 * 寄存器形式（1/2/3）：rs1_field 为 rs1 编号，src 为 rs1 的值。
 * 立即数形式（5/6/7）：rs1_field 与 src 都传入 5 位零扩展立即数 zimm。
 * 「是否写入」由 CSRRW/CSRRWI 恒写、其余在 rs1_field==0 时只读决定。 */
int csr_access(struct RISCVSIMCPUState *cpu, uint32_t addr, unsigned funct3,
               unsigned rs1_field, uint32_t src, uint32_t *old);

#endif
