#include "core_internal.h"
#include "cpu_state.h"
#include <assert.h>

void in_core_invalidate_reservation(INCore *core, uint32_t address, unsigned width)
{
    RISCVSIMCPUState *cpu = core->simcpu;
    /* 保守策略：本 hart 对保留字的任意字节写入也使保留失效。 */
    if (cpu->reservation_valid &&
        (uint64_t)address < (uint64_t)cpu->reservation_addr + 4 &&
        (uint64_t)cpu->reservation_addr < (uint64_t)address + width)
        cpu->reservation_valid = 0;
}

static uint32_t atomic_result(unsigned funct5, uint32_t old, uint32_t operand)
{
    /* 翻转符号位后比较，避免将超范围 unsigned 转换为 signed。 */
    int less = (old ^ 0x80000000u) < (operand ^ 0x80000000u);
    switch (funct5)
    {
    case 0x00:
        return old + operand;
    case 0x01:
        return operand;
    case 0x04:
        return old ^ operand;
    case 0x08:
        return old | operand;
    case 0x0c:
        return old & operand;
    case 0x10:
        return less ? old : operand;
    case 0x14:
        return less ? operand : old;
    case 0x18:
        return old < operand ? old : operand;
    case 0x1c:
        return old < operand ? operand : old;
    default:
        assert(0 && "illegal AMO reached MEM");
        return 0;
    }
}

/* 固定一拍同步 RAM；当前只有此 hart 能访问数据端口。 */
static uint32_t read_word(const INCore *core, uint32_t address)
{
    uint32_t value = 0;
    for (unsigned j = 0; j < 4; ++j)
        value |= (uint32_t)core->data[(size_t)address + j] << (8 * j);
    return value;
}

static void write_word(INCore *core, uint32_t address, uint32_t value)
{
    for (unsigned j = 0; j < 4; ++j)
        core->data[(size_t)address + j] = (uint8_t)(value >> (8 * j));
    in_core_invalidate_reservation(core, address, 4);
}

void in_core_atomic_memory(INCore *core)
{
    CPUStage out = core->memory;
    InsnLatch *l = &out.latch;
    unsigned funct5 = l->insn >> 27;
    RISCVSIMCPUState *cpu = core->simcpu;
    if (l->atomic_phase == ATOMIC_READ)
    {
        if (l->mem_addr & 3)
            l->exception = funct5 == 2 ? 5 : 7; /* load / store-AMO misaligned */
        else if (!core->data || (uint64_t)l->mem_addr + 4 > core->data_size ||
                 (uint64_t)l->mem_addr + 4 > UINT64_C(0x100000000))
            l->exception = funct5 == 2 ? 6 : 8; /* load / store-AMO access fault */
        if (l->exception)
        {
            l->tval = l->mem_addr;
            core->next_commit = out;
            return;
        }
        /* LR、SC 和普通 AMO 共用 OPCODE_AMO；funct5=3 仅表示 SC.W。 */
        if (funct5 == 3) /* SC 第一拍：检查并消费保留；第二拍完成条件写入。 */
        {
            l->result = !(cpu->reservation_valid && cpu->reservation_addr == l->mem_addr); /* SC状态 */
            cpu->reservation_valid = 0;
            l->mem_value = l->rs2_value;
            l->atomic_phase = ATOMIC_WRITE;
        }
        else
        {
            l->result = read_word(core, l->mem_addr);
            if (funct5 == 2) /* LR：同步 RAM 读取和建立保留占一拍。 */
            {
                cpu->reservation_addr = l->mem_addr;
                cpu->reservation_valid = 1;
                core->next_commit = out;
                return;
            }
            l->atomic_phase = ATOMIC_MODIFY;
        }
    }
    else if (l->atomic_phase == ATOMIC_MODIFY)
    {
        l->mem_value = atomic_result(funct5, l->result, l->rs2_value);
        l->atomic_phase = ATOMIC_WRITE;
    }
    else
    {
        /* 此时较老指令已提交，年轻指令无法访问 MEM，也没有异步异常。
         * 写入发生于 MEM 最后一拍；下一拍 WB 只写寄存器并退休。
         * 单 hart 阻塞端口提供比 aq/rl 更强的排序，不额外插入延迟。
         */
        assert(l->atomic_phase == ATOMIC_WRITE);
        if (funct5 != 3 || l->result == 0)
            write_word(core, l->mem_addr, l->mem_value);
        core->next_commit = out;
        return;
    }
    core->next_memory = out;
    core->memory_stalled = 1;
    core->stats.memory_stalls++;
}
