#include "core_internal.h"
#include "../utils/trace.h"

void in_core_fetch(INCore *core)
{
    core->fetch = (CPUStage){0};
    if (core->fetch_done)
        return;
    uint32_t pc = core->fetch_pc;
    uint32_t offset = pc - core->program_base;
    /* 固定数组的末尾是演示环境的结束标记，不是一条 ISA 指令。 */
    if (!(pc & 3) && (uint64_t)offset == (uint64_t)core->program_size * 4)
    {
        core->fetch_done = 1;
        return;
    }
    core->fetch.has_data = 1;
    InsnLatch *l = &core->fetch.latch;
    l->pc = pc;
    if (pc & 3) /* 地址未对齐 */
    {
        l->exception = 1;
        l->tval = pc;
    }
    else if (pc < core->program_base || offset / 4 >= core->program_size) /* 地址溢出 */
    {
        l->exception = 2;
        l->tval = pc;
    }
    else
    {
        core->fetch_index = offset / 4;
        if (core->bus.read) {
            if (core->bus.probe(core->bus.opaque, pc, 4, 2) ||
                core->bus.read(core->bus.opaque, pc, 4, &l->insn)) {
                l->exception = 2; l->tval = pc;
            }
        } else {
            l->insn = core->program[core->fetch_index];
        }
        core->fetch_index++;
    }
    core->fetch_pc = pc + 4;
    trace_stage(core, "IF", core->fetch);
    core->next_decode = core->fetch;
    core->fetch = (CPUStage){0};
}
