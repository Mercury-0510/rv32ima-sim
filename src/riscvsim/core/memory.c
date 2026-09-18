#include "core_internal.h"
#include "../utils/trace.h"

void in_core_memory(INCore *core)
{
    if (!core->memory.has_data)
        return;
    trace_stage(core, "MEM", core->memory);
    if (!core->memory.latch.exception &&
        (core->memory.latch.insn & OPCODE_MASK) == OPCODE_AMO)
    {
        in_core_atomic_memory(core);
        return;
    }
    core->next_commit = core->memory;
    InsnLatch *l = &core->next_commit.latch;
    /* opcode 为 insn[6:0]；funct3 为 insn[14:12]，用于细分指令类型。 */
    unsigned op = l->insn & OPCODE_MASK, f3 = (l->insn >> 12) & 7;
    if (l->exception || (op != OPCODE_LOAD && op != OPCODE_STORE))
        return;
    unsigned width = 1u << (f3 & 3); /* 低两位决定字节数：1、2、4；load 的 bit 2 区分无符号类型。 */
    uint32_t address = l->result;
    l->mem_addr = address; /* 保留有效地址：load 之后 result 会承载读回值 */
    if (address & (width - 1)) /* 地址未对齐 */
        l->exception = op == OPCODE_LOAD ? 5 : 7;
    else if (core->bus.probe
             ? core->bus.probe(core->bus.opaque, address, width, op == OPCODE_STORE)
             : (!core->data || (uint64_t)address + width > core->data_size ||
                (uint64_t)address + width > UINT64_C(0x100000000))) /* 地址越界 */
        l->exception = op == OPCODE_LOAD ? 6 : 8;
    if (l->exception)
    {
        l->tval = address;
        return;
    }
    if (op == OPCODE_LOAD) /* load 在本阶段读取*/
    {
        if (core->bus.read) return; /* 带副作用的总线读延迟到 WB */
        uint32_t value = 0;
        for (unsigned j = 0; j < width; ++j)
            value |= (uint32_t)core->data[address + j] << (j * 8);
        /* 符号扩展 */
        if (f3 == 0)
            value = (value ^ 0x80u) - 0x80u;
        if (f3 == 1)
            value = (value ^ 0x8000u) - 0x8000u;
        l->result = value;
    }
}
