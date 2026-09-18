#include "core_internal.h"
#include "cpu_state.h"

/* 无符号运算实现符号扩展，避免依赖宿主机的有符号右移。 */
static uint32_t sext(uint32_t value, unsigned bits)
{
    uint32_t sign = UINT32_C(1) << (bits - 1);
    return (value ^ sign) - sign;
}

static int pending(CPUStage stage, unsigned reg)
{
    return reg && stage.has_data && stage.latch.writes_rd &&
           stage.latch.rd == reg;
}

void in_core_decode(INCore *core)
{
    if (!core->decode.has_data)
        return;
    CPUStage out = core->decode; /* 可能存在数据冒险 */
    InsnLatch *l = &out.latch;
    uint32_t i = l->insn;
    /* 提取 opcode[6:0]、funct3[14:12] 和 funct7[31:25]。 */
    unsigned op = i & OPCODE_MASK, f3 = (i >> 12) & 7, f7 = i >> 25;
    int use1 = 0, use2 = 0, legal = 1;
    l->rd = (i >> 7) & 31;   /* insn[11:7]：5 位寄存器编号。 */
    l->rs1 = (i >> 15) & 31; /* insn[19:15]：5 位寄存器编号。 */
    l->rs2 = (i >> 20) & 31; /* insn[24:20]：5 位寄存器编号。 */
    if (l->exception)
    {
        core->next_execute = out;
        return;
    }
    switch (op)
    {
    case OPCODE_LUI:
    case OPCODE_AUIPC: /* LUI, AUIPC */
        l->writes_rd = 1;
        l->imm = i & 0xfffff000u; /* U 型：保留高 20 位，低 12 位为 0。 */
        break;
    case OPCODE_JAL: /* JAL */
        l->writes_rd = 1;
        /* J 型：拼接 imm[20|19:12|11|10:1]，最低位补 0，再符号扩展。 */
        l->imm = sext(((i >> 31) << 20) | (((i >> 12) & 255) << 12) |
                          (((i >> 20) & 1) << 11) | (((i >> 21) & 1023) << 1),
                      21);
        break;
    case OPCODE_JALR: /* JALR */
        use1 = l->writes_rd = 1;
        l->imm = sext(i >> 20, 12); /* I 型：insn[31:20] 按 12 位符号扩展。 */
        legal = f3 == 0;
        break;
    case OPCODE_BRANCH: /* branches */
        use1 = use2 = 1;
        /* B 型：拼接 imm[12|11|10:5|4:1]，最低位补 0，再符号扩展。 */
        l->imm = sext(((i >> 31) << 12) | (((i >> 7) & 1) << 11) |
                          (((i >> 25) & 63) << 5) | (((i >> 8) & 15) << 1),
                      13);
        legal = f3 != 2 && f3 != 3;
        break;
    case OPCODE_LOAD: /* loads */
        use1 = l->writes_rd = 1;
        l->imm = sext(i >> 20, 12); /* I 型：insn[31:20] 按 12 位符号扩展。 */
        legal = f3 == 0 || f3 == 1 || f3 == 2 || f3 == 4 || f3 == 5;
        break;
    case OPCODE_STORE: /* stores */
        use1 = use2 = 1;
        /* S 型：insn[31:25] 和 insn[11:7] 拼成 12 位有符号偏移。 */
        l->imm = sext(((i >> 25) << 5) | ((i >> 7) & 31), 12);
        legal = f3 <= 2;
        break;
    case OPCODE_OP_IMM: /* immediate ALU */
        use1 = l->writes_rd = 1;
        l->imm = sext(i >> 20, 12); /* I 型：insn[31:20] 按 12 位符号扩展。 */
        /* 移位立即数指令还需检查高位：SLLI/SRLI 为 0000000，SRAI 为 0100000。 */
        if (f3 == 1)
            legal = f7 == 0;
        if (f3 == 5)
            legal = f7 == 0 || f7 == 0x20;
        break;
    case OPCODE_OP: /* register ALU */
        use1 = use2 = l->writes_rd = 1;
        /* funct7=0000001 为 M 扩展；仅 SUB/SRA 允许 0100000。 */
        legal = f7 == 0 || f7 == 1 || (f7 == 0x20 && (f3 == 0 || f3 == 5));
        break;
    case OPCODE_AMO:
        use1 = use2 = l->writes_rd = 1;
        /* funct5 不包含 aq/rl；只实现 RV32A 的 .W 编码。 */
        switch (i >> 27)
        {
        case 0x02: /* LR.W 不读取 rs2，其编码必须为零。 */
            use2 = 0;
            legal = f3 == 2 && l->rs2 == 0;
            break;
        case 0x03: /* SC.W 的 rd 是成功/失败状态。 */
        case 0x00:
        case 0x01:
        case 0x04:
        case 0x08:
        case 0x0c:
        case 0x10:
        case 0x14:
        case 0x18:
        case 0x1c:
            legal = f3 == 2;
            break;
        default:
            legal = 0;
        }
        break;
    case OPCODE_MISC_MEM: /* FENCE：单核同步 RAM 不需要额外动作。 */
        legal = f3 == 0;
        break;
    case OPCODE_SYSTEM:
        if (f3 == 0)
        {
            if (i == 0x00000073u)
                l->exception = 12; /* M-mode ECALL, cause 11 */
            else if (i == 0x00100073u)
                l->exception = 4; /* breakpoint */
            else
                legal = 0; /* MRET/SRET/WFI 留待 M2 实现 */
            l->tval = i == 0x00100073u ? l->pc : 0;
        }
        else if (f3 != 4) /* funct3 1/2/3 寄存器形式，5/6/7 立即数形式 */
        {
            l->csr_addr = i >> 20; /* CSR 编号 insn[31:20] */
            l->writes_rd = 1;      /* CSR 指令把旧值写回 rd */
            use1 = f3 <= 3;        /* 立即数形式用 insn[19:15] 作为 zimm */
        }
        else
            legal = 0; /* funct3=4 未定义 */
        break;
    default:
        legal = 0;
    }
    if (!legal)
    {
        l->exception = 3; /* illegal instruction, cause 2 */
        l->tval = i;
    }
    /* WB 在 ID 之前写寄存器，因此只等待 EX/MEM 中的生产者。 */
    if (!l->exception &&
        ((use1 && (pending(core->execute, l->rs1) || pending(core->memory, l->rs1))) ||
         (use2 && (pending(core->execute, l->rs2) || pending(core->memory, l->rs2)))))
    {
        core->stalled = 1;
        core->stats.stalls++;
        core->next_decode = core->decode; /* 保持 ID，向 EX 插入气泡 */
        return;
    }
    l->rs1_value = l->rs1 ? core->simcpu->regs[l->rs1] : 0; /* reg0单独处理 */
    l->rs2_value = l->rs2 ? core->simcpu->regs[l->rs2] : 0;
    core->next_execute = out;
}
