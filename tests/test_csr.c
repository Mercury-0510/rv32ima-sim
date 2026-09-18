#include "test_util.h"
#include "core/csr.h"
#include <string.h>

static RISCVSIMCPUState cpu;
static unsigned cases;

static void reset(uint32_t priv)
{
    memset(&cpu, 0, sizeof(cpu));
    cpu.priv = priv;
    cases++;
}

int main(void)
{
    uint32_t v;

    /* 1. M 模式可读写 mscratch，并读回写入值。 */
    reset(PRIV_M);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RW, 1, 0x1234, &v) == 0 && v == 0);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 0x1234);

    /* 2. CSRRS 置位、CSRRC 清位，且返回旧值。 */
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RS, 1, 0x00f0, &v) == 0 && v == 0x1234);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 0x12f4);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RC, 1, 0x0010, &v) == 0 && v == 0x12f4);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 0x12e4);

    /* 3. rs1 域为 0 时 CSRRS 只读；CSRRWI 即使 zimm=0 也写。 */
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RS, 0, 0, &v) == 0 && v == 0x12e4);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 0x12e4);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RWI, 0, 7, &v) == 0 && v == 0x12e4);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 7);

    /* 4. 立即数形式：zimm=0 不改写，zimm!=0 生效。 */
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RSI, 0, 0, &v) == 0 && v == 7);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RSI, 8, 8, &v) == 0 && v == 7);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 15);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RCI, 1, 1, &v) == 0 && v == 15);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == 0 && v == 14);

    /* 5. 只读 CSR：可读，写入非法；只读访问仍然合法。 */
    reset(PRIV_M);
    CHECK(csr_read(&cpu, CSR_CYCLE, &v) == 0);
    CHECK(csr_access(&cpu, CSR_CYCLE, CSR_F3_RW, 1, 1, &v) == -1);
    CHECK(csr_access(&cpu, CSR_CYCLE, CSR_F3_RS, 1, 1, &v) == -1);
    CHECK(csr_access(&cpu, CSR_CYCLE, CSR_F3_RS, 0, 0, &v) == 0);

    /* 6. 只读 ID 寄存器可读、写入非法。 */
    reset(PRIV_M);
    CHECK(csr_read(&cpu, CSR_MISA, &v) == 0 && v == MISA_VALUE);
    CHECK(csr_read(&cpu, CSR_MHARTID, &v) == 0 && v == 0);
    CHECK(csr_access(&cpu, CSR_MVENDORID, CSR_F3_RW, 1, 0, &v) == -1);

    /* 7. 特权级：U 模式不能访问 M CSR。 */
    reset(PRIV_U);
    CHECK(csr_read(&cpu, CSR_MSTATUS, &v) == -1);
    CHECK(csr_access(&cpu, CSR_MSCRATCH, CSR_F3_RW, 1, 1, &v) == -1);

    /* 8. U 模式计数器访问受 mcounteren 控制。 */
    CHECK(csr_read(&cpu, CSR_CYCLE, &v) == -1);
    cpu.csr[CSR_MCOUNTEREN] = COUNTER_CYCLE | COUNTER_INSTRET;
    CHECK(csr_read(&cpu, CSR_CYCLE, &v) == 0);
    CHECK(csr_read(&cpu, CSR_INSTRET, &v) == 0);
    CHECK(csr_read(&cpu, CSR_TIME, &v) == -1);

    /* 9. S 模式可访问 S CSR；M CSR 仍非法。 */
    reset(PRIV_S);
    CHECK(csr_access(&cpu, CSR_SSCRATCH, CSR_F3_RW, 1, 0xabc, &v) == 0);
    CHECK(csr_read(&cpu, CSR_SSCRATCH, &v) == 0 && v == 0xabc);
    CHECK(csr_read(&cpu, CSR_MSCRATCH, &v) == -1);

    /* 10. misa 为 WARL：写入不报错，但取值保持不变。 */
    reset(PRIV_M);
    CHECK(csr_access(&cpu, CSR_MISA, CSR_F3_RWI, 0, 0, &v) == 0 && v == MISA_VALUE);
    CHECK(csr_read(&cpu, CSR_MISA, &v) == 0 && v == MISA_VALUE);

    printf("CSR: %u directed cases passed\n", cases);
    return 0;
}
