/* 只在 ENABLE_TRACE=1 时参与编译。关闭时 trace.h 把两个函数定义成宏，本文件
 * 里的函数定义会展开成非法语句，所以由 CMakeLists.txt 从源列表里整体去掉，
 * 而不是在本文件里加条件编译。 */
#include "trace.h"
#include "../core/core.h"
#include "../core/cpu_state.h"
#include "../core/opcodes.h"
#include <inttypes.h>
#include <stdio.h>

void trace_stage(const INCore *core, const char *name, CPUStage stage)
{
    if (!core->trace || !stage.has_data) return;
    printf("cycle=%" PRIu64 " %-3s pc=0x%08" PRIx32 " insn=0x%08" PRIx32 "\n",
           core->simcpu->clock + 1, name, stage.latch.pc, stage.latch.insn);
}

void trace_commit(const INCore *core, int trap)
{
    if (!core->commit_trace) return;
    const InsnLatch *l = &core->commit.latch;
    unsigned op = l->insn & OPCODE_MASK;
    unsigned width = op == OPCODE_STORE ? 1u << ((l->insn >> 12) & 3) : 0;
    unsigned mask = !trap && width ? (1u << width) - 1u : 0;
    int wen = !trap && l->writes_rd && l->rd;
    uint32_t data = mask ? l->rs2_value : 0;
    if (mask && width < 4) data &= (UINT32_C(1) << (width * 8)) - 1;
    fprintf(core->commit_trace,
        "{\"type\":\"%s\",\"cycle\":%" PRIu64 ",\"seq\":%" PRIu64
        ",\"pc\":\"0x%08" PRIx32 "\",\"insn\":\"0x%08" PRIx32
        "\",\"wen\":%d,\"rd\":%u,\"wdata\":\"0x%08" PRIx32
        "\",\"mem_addr\":\"0x%08" PRIx32 "\",\"mem_wmask\":%u,"
        "\"mem_wdata\":\"0x%08" PRIx32 "\",\"trap\":%s,\"cause\":%u,"
        "\"tval\":\"0x%08" PRIx32 "\"}\n",
        trap ? "trap" : "commit", core->simcpu->clock + 1,
        core->stats.retired, l->pc, l->insn, wen, wen ? l->rd : 0,
        wen ? l->result : 0, (op == OPCODE_LOAD || op == OPCODE_STORE) ? l->mem_addr : 0,
        mask, data, trap ? "true" : "false",
        trap ? core->simcpu->trap_cause : 0, trap ? l->tval : 0);
}
