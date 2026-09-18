#include "core_internal.h"
#include "csr.h"
#include "cpu_state.h"
#include "../utils/trace.h"

/* 异常在 WB 统一报告：较老指令已提交，年轻指令被冲刷。 */
static int report_trap(INCore *core, InsnLatch *l)
{
    core->simcpu->trapped = core->halted = 1;
    core->simcpu->trap_cause = (uint32_t)(l->exception - 1);
    core->simcpu->trap_pc = l->pc;
    core->simcpu->trap_value = l->tval;
    trace_commit(core, 1);
    return 1;
}

int in_core_commit(INCore *core)
{
    if (!core->commit.has_data)
        return 0;
    trace_stage(core, "WB", core->commit);
    InsnLatch *l = &core->commit.latch;
    if (l->exception)
        return report_trap(core, l);
    unsigned op = l->insn & OPCODE_MASK;
    /* Bus reads (including MMIO) happen only at retirement. A younger load
     * must never consume a device value ahead of an older faulting access. */
    if (op == OPCODE_LOAD && core->bus.read) {
        unsigned f3 = (l->insn >> 12) & 7, width = 1u << (f3 & 3);
        uint32_t value;
        if (core->bus.read(core->bus.opaque, l->mem_addr, width, &value)) {
            l->exception = 6; l->tval = l->mem_addr;
            return report_trap(core, l);
        }
        if (f3 == 0) value = (value ^ 0x80u) - 0x80u;
        if (f3 == 1) value = (value ^ 0x8000u) - 0x8000u;
        l->result = value;
    }
    /* store 写入延迟到 WB，保证异常精确且不写入错误路径数据。 */
    if (op == OPCODE_STORE)
    {
        unsigned width = 1u << ((l->insn >> 12) & 3); /* 根据 funct3 低两位计算写入字节数：SB=1、SH=2、SW=4。 */
        size_t address = l->result;
        if (core->bus.write) {
            if (core->bus.write(core->bus.opaque, (uint32_t)address, width, l->rs2_value)) {
                l->exception = 8; l->tval = (uint32_t)address;
                return report_trap(core, l);
            }
        } else {
            for (unsigned j = 0; j < width; ++j)
                core->data[address + j] = (uint8_t)(l->rs2_value >> (j * 8));
            in_core_invalidate_reservation(core, (uint32_t)address, width);
        }
    }
    /* CSR 读改写延迟到 WB：与 store 一致，保证异常精确。 */
    if (op == OPCODE_SYSTEM)
    {
        unsigned f3 = (l->insn >> 12) & 7;
        if (f3 != 0)
        {
            uint32_t src = f3 > 3 ? l->rs1 : l->rs1_value; /* 立即数形式取 zimm */
            uint32_t old_value = 0;
            if (csr_access(core->simcpu, l->csr_addr, f3, l->rs1, src, &old_value))
            {
                l->exception = 3; /* illegal instruction, cause 2 */
                l->tval = l->insn;
                return report_trap(core, l);
            }
            l->result = old_value; /* rd 写回 CSR 旧值 */
        }
    }
    if (l->writes_rd && l->rd)
        core->simcpu->regs[l->rd] = l->result;
    core->simcpu->regs[0] = 0;
    core->stats.retired++;
    core->simcpu->instret++;
    trace_commit(core, 0);
    if (core->config.stop_pc_valid && l->pc == core->config.stop_pc) {
        core->halted = 1;
        return 1;
    }
    return 0;
}
